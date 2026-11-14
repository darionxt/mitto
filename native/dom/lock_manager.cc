#include "mitto/dom/lock_manager.h"

#include <tuple>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/unguessable_token.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_lock_manager.h"
#include "mitto/bindings/v8_window.h"
#include "mitto/dom/abort_signal.h"
#include "mitto/dom/document.h"
#include "mitto/dom/lock.h"
#include "mitto/dom/script_values.h"
#include "mitto/dom/window.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

constexpr char kInvalidStateErrorMessage[] = "The document is not active.";
constexpr char kAccessDeniedMessage[] =
    "Access to the Locks API is denied in this context.";

void Post(base::OnceClosure task) {
  // What a document asks of the lock service goes to it as IPC, which belongs
  // to no frame.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
                                                              std::move(task));
}

url::Origin OriginOf(Window& window) {
  return url::Origin::Create(
      GURL(base::UTF16ToUTF8(window.GetDocument().UrlForBinding())));
}

bool WindowHasContext(v8::Isolate* isolate, v8::Local<v8::Context> context) {
  const Window* const window =
      bindings::V8Window::ToWrappable(isolate, context->Global());
  return window && window->HasContext();
}

v8::Local<v8::Object> LockInfoObject(v8::Isolate* isolate,
                                     const LockService::Info& info) {
  // A dictionary's members, in the order of their names.
  return PlainObject(
      isolate,
      {{"clientId", bindings::ToV8StringFromUtf8(isolate, info.client_id)},
       {"mode", bindings::ToV8String(isolate, Lock::ModeName(info.mode))},
       {"name", bindings::ToV8StringFromUtf8(isolate, info.name)}});
}

v8::Local<v8::Array> LockInfoArray(
    v8::Isolate* isolate,
    const std::vector<LockService::Info>& infos) {
  std::vector<v8::Local<v8::Value>> items;
  items.reserve(infos.size());
  for (const LockService::Info& info : infos) {
    items.push_back(LockInfoObject(isolate, info));
  }
  return v8::Array::New(isolate, items.data(), items.size());
}

}  // namespace

// The manager's end of its pipe to the service: it runs what the service says
// in the window's context, while the manager and the context last, and lets a
// granted lock go otherwise, as a closed pipe does.
class LockManager::Connection final : public LockService::Client {
 public:
  Connection(v8::Isolate* isolate,
             v8::Local<v8::Context> context,
             LockManager* manager,
             std::string origin)
      : isolate_(isolate),
        context_(isolate, context),
        manager_(manager),
        origin_(std::move(origin)) {}

  // The context went.
  void Close() {
    context_.Reset();
    manager_.Clear();
  }

  // Runs `run` with the manager in its context, in a task's scope.
  template <typename Run>
  bool RunInContext(Run run) {
    if (!manager_ || context_.IsEmpty()) {
      return false;
    }
    const v8::HandleScope handle_scope(isolate_);
    const v8::Local<v8::Context> context = context_.Get(isolate_);
    if (!WindowHasContext(isolate_, context)) {
      return false;
    }
    const v8::Context::Scope context_scope(context);
    const v8::MicrotasksScope microtasks_scope(
        context, v8::MicrotasksScope::kRunMicrotasks);
    run(*manager_);
    return true;
  }

  // LockService::Client:
  void Granted(int64_t lock_id) override {
    if (!RunInContext(
            [lock_id](LockManager& manager) { manager.Granted(lock_id); })) {
      // The handle is dropped, which lets the lock go.
      LockService::Get().ReleaseLock(origin_, lock_id);
    }
  }
  void Failed(int64_t lock_id) override {
    RunInContext([lock_id](LockManager& manager) { manager.Failed(lock_id); });
  }
  void Broken(int64_t lock_id) override {
    RunInContext([lock_id](LockManager& manager) { manager.Broken(lock_id); });
  }

 private:
  ~Connection() override = default;

  const raw_ptr<v8::Isolate> isolate_;
  v8::Global<v8::Context> context_;
  cppgc::WeakPersistent<LockManager> manager_;
  const std::string origin_;
};

// LockManager::LockRequestImpl: a request until it is granted, fails or is
// aborted.
class LockManager::LockRequest final
    : public cppgc::GarbageCollected<LockRequest> {
 public:
  LockRequest(v8::Isolate* isolate,
              v8::Local<v8::Function> callback,
              v8::Local<v8::Promise::Resolver> resolver,
              std::u16string name,
              LockService::Mode mode)
      : callback_(isolate, callback),
        resolver_(isolate, resolver),
        name_(std::move(name)),
        mode_(mode) {}

  void Trace(cppgc::Visitor* visitor) const {
    visitor->Trace(callback_);
    visitor->Trace(resolver_);
    visitor->Trace(signal_);
    visitor->Trace(abort_algorithm_);
  }

  // LockRequestImpl's abort handle: while the request waits, it keeps its
  // signal and the algorithm that aborts it, so that a signal that only the
  // request holds, such as one of AbortSignal.any(), still aborts it.
  void SetAbortHandle(AbortSignal* signal, AbortSignal::Algorithm* algorithm) {
    signal_ = signal;
    abort_algorithm_ = algorithm;
  }
  void ClearAbortHandle() {
    signal_.Clear();
    abort_algorithm_.Clear();
  }

  v8::Local<v8::Function> callback(v8::Isolate* isolate) const {
    return callback_.Get(isolate);
  }
  v8::Local<v8::Promise::Resolver> resolver(v8::Isolate* isolate) const {
    return resolver_.Get(isolate);
  }
  const std::u16string& name() const { return name_; }
  LockService::Mode mode() const { return mode_; }
  int64_t lock_id() const { return lock_id_; }
  void set_lock_id(int64_t lock_id) { lock_id_ = lock_id; }

 private:
  v8::TracedReference<v8::Function> callback_;
  v8::TracedReference<v8::Promise::Resolver> resolver_;
  const std::u16string name_;
  const LockService::Mode mode_;
  int64_t lock_id_ = 0;
  cppgc::Member<AbortSignal> signal_;
  cppgc::Member<AbortSignal::Algorithm> abort_algorithm_;
};

// The abort algorithm of a request with a signal: LockRequestImpl::Abort().
class LockManager::AbortRequest final : public AbortSignal::Algorithm {
 public:
  AbortRequest(LockManager* manager, LockRequest* request, AbortSignal* signal)
      : manager_(manager), request_(request), signal_(signal) {}

  void Run() override {
    LockManager* const manager = manager_.Get();
    LockRequest* const request = request_.Get();
    // An abort once the lock is granted does nothing.
    if (!manager || !request ||
        manager->TakePendingRequest(request->lock_id()) != request) {
      return;
    }
    LockService::Get().ReleaseLock(manager->origin_, request->lock_id());
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    std::ignore = request->resolver(isolate)->Reject(
        isolate->GetCurrentContext(), signal_->Reason());
  }

  void Trace(cppgc::Visitor* visitor) const override {
    AbortSignal::Algorithm::Trace(visitor);
    visitor->Trace(manager_);
    visitor->Trace(request_);
    visitor->Trace(signal_);
  }

 private:
  const cppgc::WeakMember<LockManager> manager_;
  const cppgc::WeakMember<LockRequest> request_;
  const cppgc::Member<AbortSignal> signal_;
};

LockManager::LockManager(Window& window)
    : origin_(OriginOf(window).Serialize()),
      client_id_(base::UnguessableToken::Create().ToString()),
      connection_(base::MakeRefCounted<Connection>(
          v8::Isolate::GetCurrent(),
          v8::Isolate::GetCurrent()->GetCurrentContext(),
          this,
          origin_)) {
  if (window.HasContext()) {
    window.AddContextDestroyedCallback(base::BindOnce(
        [](cppgc::WeakPersistent<LockManager> manager) {
          if (manager) {
            manager->ContextDestroyed();
          }
        },
        cppgc::WeakPersistent<LockManager>(this)));
  } else {
    connection_->Close();
  }
}

LockManager::~LockManager() = default;

const bindings::WrapperTypeInfo* LockManager::GetWrapperTypeInfo() const {
  return &bindings::V8LockManager::kWrapperTypeInfo;
}

void LockManager::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  pending_requests_.Trace(visitor);
  held_locks_.Trace(visitor);
}

bool LockManager::CheckAccess(bindings::ExceptionState& exception_state) const {
  // The observed context goes with the window's.
  if (context_destroyed_) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      kInvalidStateErrorMessage);
    return false;
  }
  // SecurityOrigin::CanAccessLocks(): not for an opaque origin.
  if (origin_ == "null") {
    exception_state.ThrowDOMException("SecurityError", kAccessDeniedMessage);
    return false;
  }
  return true;
}

v8::Local<v8::Promise> LockManager::Request(
    const std::u16string& name,
    v8::Local<v8::Function> callback,
    bindings::ExceptionState& exception_state) {
  return Request(name, bindings::LockOptions(), callback, exception_state);
}

v8::Local<v8::Promise> LockManager::Request(
    const std::u16string& name,
    const bindings::LockOptions& options,
    v8::Local<v8::Function> callback,
    bindings::ExceptionState& exception_state) {
  if (!CheckAccess(exception_state)) {
    return {};
  }
  const LockService::Mode mode = options.mode == u"shared"
                                     ? LockService::Mode::kShared
                                     : LockService::Mode::kExclusive;
  if (name.starts_with(u'-')) {
    exception_state.ThrowDOMException("NotSupportedError",
                                      "Names cannot start with '-'.");
    return {};
  }
  if (options.steal && options.if_available) {
    exception_state.ThrowDOMException(
        "NotSupportedError",
        "The 'steal' and 'ifAvailable' options cannot be used together.");
    return {};
  }
  if (options.steal && mode != LockService::Mode::kExclusive) {
    exception_state.ThrowDOMException(
        "NotSupportedError",
        "The 'steal' option may only be used with 'exclusive' locks.");
    return {};
  }
  AbortSignal* const signal = options.signal ? *options.signal : nullptr;
  if (signal && options.if_available) {
    exception_state.ThrowDOMException(
        "NotSupportedError",
        "The 'signal' and 'ifAvailable' options cannot be used together.");
    return {};
  }
  if (signal && options.steal) {
    exception_state.ThrowDOMException(
        "NotSupportedError",
        "The 'signal' and 'steal' options cannot be used together.");
    return {};
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return {};
  }
  if (signal && signal->Aborted()) {
    std::ignore = resolver->Reject(context, signal->Reason());
    return resolver->GetPromise();
  }
  const LockService::WaitMode wait =
      options.steal          ? LockService::WaitMode::kPreempt
      : options.if_available ? LockService::WaitMode::kNoWait
                             : LockService::WaitMode::kWait;
  LockRequest* const request = bindings::MakeGarbageCollected<LockRequest>(
      isolate, callback, resolver, name, mode);
  CheckStorageAccessAllowed(base::BindOnce(
      [](cppgc::WeakPersistent<LockManager> manager,
         cppgc::Persistent<LockRequest> request, LockService::WaitMode wait,
         cppgc::Persistent<AbortSignal> signal) {
        if (manager) {
          manager->RequestImpl(request.Get(), wait, signal.Get());
        }
      },
      cppgc::WeakPersistent<LockManager>(this),
      cppgc::Persistent<LockRequest>(request), wait,
      cppgc::Persistent<AbortSignal>(signal)));
  return resolver->GetPromise();
}

void LockManager::CheckStorageAccessAllowed(base::OnceClosure callback) {
  if (cached_allowed_) {
    std::move(callback).Run();
    return;
  }
  // LocalFrame::AllowStorageAccessAndNotify() asks the browser, whose answer
  // comes from a task of its own; a Page's documents may always use storage.
  Post(base::BindOnce(
      [](scoped_refptr<Connection> connection, base::OnceClosure callback) {
        Post(base::BindOnce(
            [](scoped_refptr<Connection> connection,
               base::OnceClosure callback) {
              connection->RunInContext([&callback](LockManager& manager) {
                manager.cached_allowed_ = true;
                std::move(callback).Run();
              });
            },
            std::move(connection), std::move(callback)));
      },
      connection_, std::move(callback)));
}

void LockManager::RequestImpl(LockRequest* request,
                              LockService::WaitMode wait,
                              AbortSignal* signal) {
  if (context_destroyed_) {
    return;
  }
  pending_requests_.push_back(request);
  // The lock's ID is known before the request reaches the service, and before
  // anything can abort it.
  request->set_lock_id(LockService::Get().RequestLock(
      connection_, origin_, client_id_, base::UTF16ToUTF8(request->name()),
      request->mode(), wait));
  if (signal) {
    AbortRequest* const algorithm =
        bindings::MakeGarbageCollected<AbortRequest>(this, request, signal);
    signal->AddAlgorithm(algorithm);
    request->SetAbortHandle(signal, algorithm);
  }
}

LockManager::LockRequest* LockManager::TakePendingRequest(int64_t lock_id) {
  LockRequest* found = nullptr;
  pending_requests_.EraseIf([&found, lock_id](LockRequest* request) {
    if (found || request->lock_id() != lock_id) {
      return false;
    }
    found = request;
    return true;
  });
  // Granted, failed or aborted, it no longer needs its signal.
  if (found) {
    found->ClearAbortHandle();
  }
  return found;
}

// LockRequestImpl::Granted().
void LockManager::Granted(int64_t lock_id) {
  LockRequest* const request = TakePendingRequest(lock_id);
  if (!request) {
    // Aborted meanwhile: the handle is dropped, which lets the lock go.
    LockService::Get().ReleaseLock(origin_, lock_id);
    return;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  Lock* const lock = bindings::MakeGarbageCollected<Lock>(
      request->name(), request->mode(), lock_id, this);
  held_locks_.push_back(lock);
  const v8::TryCatch try_catch(isolate);
  v8::Local<v8::Value> argument = bindings::ToV8(isolate, lock);
  v8::Local<v8::Value> result;
  const bool returned =
      request->callback(isolate)
          ->Call(context, v8::Undefined(isolate), 1, &argument)
          .ToLocal(&result);
  v8::Local<v8::Promise::Resolver> held;
  if (!v8::Promise::Resolver::New(context).ToLocal(&held)) {
    return;
  }
  if (try_catch.HasCaught()) {
    if (!try_catch.CanContinue()) {
      return;
    }
    std::ignore = held->Reject(context, try_catch.Exception());
  } else if (returned) {
    std::ignore = held->Resolve(context, result);
  } else {
    return;
  }
  lock->HoldUntil(context, held->GetPromise(), request->resolver(isolate));
}

// LockRequestImpl::Failed(): the callback gets null.
void LockManager::Failed(int64_t lock_id) {
  LockRequest* const request = TakePendingRequest(lock_id);
  if (!request) {
    return;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::TryCatch try_catch(isolate);
  v8::Local<v8::Value> argument = v8::Null(isolate);
  v8::Local<v8::Value> result;
  const bool returned =
      request->callback(isolate)
          ->Call(context, v8::Undefined(isolate), 1, &argument)
          .ToLocal(&result);
  const v8::Local<v8::Promise::Resolver> resolver = request->resolver(isolate);
  if (try_catch.HasCaught()) {
    if (try_catch.CanContinue()) {
      std::ignore = resolver->Reject(context, try_catch.Exception());
    }
  } else if (returned) {
    std::ignore = resolver->Resolve(context, result);
  }
}

void LockManager::Broken(int64_t lock_id) {
  for (Lock* const lock : held_locks_.ToVector()) {
    if (lock->lock_id() == lock_id) {
      lock->OnBroken(v8::Isolate::GetCurrent()->GetCurrentContext());
      return;
    }
  }
}

void LockManager::OnLockReleased(Lock* lock) {
  held_locks_.EraseIf([lock](Lock* held) { return held == lock; });
  LockService::Get().ReleaseLock(origin_, lock->lock_id());
}

v8::Local<v8::Promise> LockManager::Query(
    bindings::ExceptionState& exception_state) {
  if (!CheckAccess(exception_state)) {
    return {};
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return {};
  }
  CheckStorageAccessAllowed(base::BindOnce(
      [](scoped_refptr<Connection> connection, std::string origin,
         v8::Global<v8::Promise::Resolver> resolver) {
        // LockManager::QueryImpl(): the service's snapshot, from a task of its
        // own.
        LockService::Get().QueryState(
            std::move(origin),
            base::BindOnce(
                [](scoped_refptr<Connection> connection,
                   v8::Global<v8::Promise::Resolver> resolver,
                   std::vector<LockService::Info> pending,
                   std::vector<LockService::Info> held) {
                  connection->RunInContext([&](LockManager&) {
                    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
                    const v8::Local<v8::Object> snapshot = PlainObject(
                        isolate,
                        {{"held", LockInfoArray(isolate, held)},
                         {"pending", LockInfoArray(isolate, pending)}});
                    std::ignore = resolver.Get(isolate)->Resolve(
                        isolate->GetCurrentContext(), snapshot);
                  });
                },
                std::move(connection), std::move(resolver)));
      },
      connection_, origin_,
      v8::Global<v8::Promise::Resolver>(isolate, resolver)));
  return resolver->GetPromise();
}

// LockManager::ContextDestroyed(): the requests are cancelled, and the locks
// let go, each as its pipe closes.
void LockManager::ContextDestroyed() {
  context_destroyed_ = true;
  for (LockRequest* const request : pending_requests_.ToVector()) {
    LockService::Get().ReleaseLock(origin_, request->lock_id());
  }
  pending_requests_.clear();
  for (Lock* const lock : held_locks_.ToVector()) {
    lock->ReleaseIfHeld();
  }
  held_locks_.clear();
  connection_->Close();
}

}  // namespace mitto
