#include "mitto/dom/scheduler.h"

#include <stdint.h>

#include <optional>
#include <tuple>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_scheduler.h"
#include "mitto/bindings/v8_task_signal.h"
#include "mitto/bindings/v8_window.h"
#include "mitto/dom/abort_signal.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/scheduler_task_context.h"
#include "mitto/dom/task_signal.h"
#include "mitto/dom/window.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-traced-handle.h"

namespace mitto {
namespace {

using Priority = SchedulerTaskContext::Priority;

Priority PriorityOf(const std::u16string& priority) {
  if (priority == u"user-blocking") {
    return Priority::kUserBlocking;
  }
  if (priority == u"background") {
    return Priority::kBackground;
  }
  return Priority::kUserVisible;
}

scoped_refptr<base::SingleThreadTaskRunner> RunnerFor(Priority priority,
                                                      bool continuation) {
  // The Page program always makes one before it runs a script.
  EventLoop* const event_loop = EventLoop::Get();
  CHECK(event_loop);
  return event_loop->TaskRunner(
      SchedulerTaskContext::QueueFor(priority, continuation));
}

// The context of the promise that `resolver` settles, if its window still runs
// it: a resolver of a context that has gone settles nothing.
std::optional<v8::Local<v8::Context>> ResolverContext(
    v8::Isolate* isolate,
    v8::Local<v8::Promise::Resolver> resolver) {
  v8::Local<v8::Context> context;
  if (!resolver->GetPromise()->GetCreationContext(isolate).ToLocal(&context)) {
    return std::nullopt;
  }
  const Window* const window =
      bindings::V8Window::ToWrappable(isolate, context->Global());
  if (!window || !window->HasContext()) {
    return std::nullopt;
  }
  return context;
}

// A postTask() callback, from its post until it has run or its signal aborted
// it.
class DOMTask final : public cppgc::GarbageCollected<DOMTask> {
 public:
  DOMTask(v8::Isolate* isolate,
          v8::Local<v8::Function> callback,
          v8::Local<v8::Promise::Resolver> resolver,
          SchedulerTaskContext* context)
      : callback_(isolate, callback),
        resolver_(isolate, resolver),
        context_(context) {}

  void Trace(cppgc::Visitor* visitor) const {
    visitor->Trace(callback_);
    visitor->Trace(resolver_);
    visitor->Trace(context_);
  }

  void Invoke();
  void OnAbort();

  bool finished() const { return finished_; }

 private:
  v8::TracedReference<v8::Function> callback_;
  v8::TracedReference<v8::Promise::Resolver> resolver_;
  const cppgc::Member<SchedulerTaskContext> context_;
  bool cancelled_ = false;
  bool running_async_ = false;
  bool finished_ = false;
};

// DOMTask::Invoke() and InvokeInternal(): the callback runs with no arguments
// and undefined for `this`, with its state current, and the promise settles as
// it returns or throws.
void DOMTask::Invoke() {
  if (cancelled_) {
    return;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::HandleScope handle_scope(isolate);
  const v8::Local<v8::Promise::Resolver> resolver = resolver_.Get(isolate);
  const std::optional<v8::Local<v8::Context>> context =
      ResolverContext(isolate, resolver);
  if (!context) {
    finished_ = true;
    return;
  }
  const v8::Context::Scope context_scope(*context);
  const SchedulerTaskScope task_scope(isolate, context_.Get());
  const v8::MicrotasksScope microtasks_scope(
      *context, v8::MicrotasksScope::kRunMicrotasks);
  bool pending = false;
  {
    const v8::TryCatch try_catch(isolate);
    v8::Local<v8::Value> result;
    if (callback_.Get(isolate)
            ->Call(*context, v8::Undefined(isolate), 0, nullptr)
            .ToLocal(&result)) {
      pending = result->IsPromise() &&
                result.As<v8::Promise>()->State() == v8::Promise::kPending;
      std::ignore = resolver->Resolve(*context, result);
    } else if (try_catch.HasCaught() && try_catch.CanContinue()) {
      std::ignore = resolver->Reject(*context, try_catch.Exception());
    }
  }
  // Until a pending result settles, an abort does nothing to the promise, as
  // it does nothing once the task has finished.
  running_async_ = pending;
  finished_ = !pending;
  callback_.Reset();
}

// DOMTask::OnAbort(): before the callback has run, or while it runs, the
// promise rejects with the signal's reason.
void DOMTask::OnAbort() {
  if (finished_) {
    return;
  }
  cancelled_ = true;
  if (running_async_) {
    return;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::HandleScope handle_scope(isolate);
  const v8::Local<v8::Promise::Resolver> resolver = resolver_.Get(isolate);
  const std::optional<v8::Local<v8::Context>> context =
      ResolverContext(isolate, resolver);
  if (!context) {
    return;
  }
  const v8::Context::Scope context_scope(*context);
  std::ignore = resolver->Reject(*context, context_->abort_source()->Reason());
}

// A continuation of yield(), from its post until it has run or its signal
// aborted it.
class DOMTaskContinuation final
    : public cppgc::GarbageCollected<DOMTaskContinuation> {
 public:
  DOMTaskContinuation(v8::Isolate* isolate,
                      v8::Local<v8::Promise::Resolver> resolver,
                      AbortSignal* signal)
      : resolver_(isolate, resolver), signal_(signal) {}

  void Trace(cppgc::Visitor* visitor) const {
    visitor->Trace(resolver_);
    visitor->Trace(signal_);
  }

  // DOMTaskContinuation::Invoke(): the promise resolves.
  void Invoke() {
    if (finished_) {
      return;
    }
    finished_ = true;
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    const v8::HandleScope handle_scope(isolate);
    const v8::Local<v8::Promise::Resolver> resolver = resolver_.Get(isolate);
    const std::optional<v8::Local<v8::Context>> context =
        ResolverContext(isolate, resolver);
    if (!context) {
      return;
    }
    const v8::Context::Scope context_scope(*context);
    const v8::MicrotasksScope microtasks_scope(
        *context, v8::MicrotasksScope::kRunMicrotasks);
    std::ignore = resolver->Resolve(*context, v8::Undefined(isolate));
  }

  // DOMTaskContinuation::OnAbort(): the promise rejects with the signal's
  // reason, and the continuation does not run.
  void OnAbort() {
    if (finished_) {
      return;
    }
    finished_ = true;
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    const v8::HandleScope handle_scope(isolate);
    const v8::Local<v8::Promise::Resolver> resolver = resolver_.Get(isolate);
    const std::optional<v8::Local<v8::Context>> context =
        ResolverContext(isolate, resolver);
    if (!context) {
      return;
    }
    const v8::Context::Scope context_scope(*context);
    std::ignore = resolver->Reject(*context, signal_->Reason());
  }

 private:
  v8::TracedReference<v8::Promise::Resolver> resolver_;
  const cppgc::Member<AbortSignal> signal_;
  bool finished_ = false;
};

// The abort algorithm of a task or a continuation, held weakly, as Blink binds
// it.
template <typename T>
class AbortAlgorithm final : public AbortSignal::Algorithm {
 public:
  explicit AbortAlgorithm(T* target) : target_(target) {}

  void Run() override {
    if (T* const target = target_.Get()) {
      target->OnAbort();
    }
  }

  void Trace(cppgc::Visitor* visitor) const override {
    AbortSignal::Algorithm::Trace(visitor);
    visitor->Trace(target_);
  }

 private:
  const cppgc::WeakMember<T> target_;
};

}  // namespace

Scheduler::Scheduler(Window* window) : window_(window) {}

Scheduler::~Scheduler() = default;

const bindings::WrapperTypeInfo* Scheduler::GetWrapperTypeInfo() const {
  return &bindings::V8Scheduler::kWrapperTypeInfo;
}

void Scheduler::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(window_);
}

bool Scheduler::CheckAttached(bindings::ExceptionState& exception_state) const {
  const Window* const window = window_.Get();
  if (!window || !window->HasContext()) {
    exception_state.ThrowDOMException("NotSupportedError",
                                      "Current window is detached");
    return false;
  }
  return true;
}

// DOMScheduler::postTask().
v8::Local<v8::Promise> Scheduler::PostTask(
    v8::Local<v8::Function> callback,
    const bindings::SchedulerPostTaskOptions& options,
    bindings::ExceptionState& exception_state) {
  if (!CheckAttached(exception_state)) {
    return {};
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return {};
  }
  AbortSignal* const signal = options.signal ? *options.signal : nullptr;
  if (signal && signal->Aborted()) {
    std::ignore = resolver->Reject(context, signal->Reason());
    return resolver->GetPromise();
  }
  // The priority option overrides the signal's; with neither, user-visible.
  Priority priority = Priority::kUserVisible;
  if (options.priority) {
    priority = PriorityOf(*options.priority);
  } else if (signal && signal->GetWrapperTypeInfo() ==
                           &bindings::V8TaskSignal::kWrapperTypeInfo) {
    priority = PriorityOf(static_cast<TaskSignal*>(signal)->Priority());
  }
  auto* const task_context =
      bindings::MakeGarbageCollected<SchedulerTaskContext>(window_.Get(),
                                                           signal, priority);
  DOMTask* const task = bindings::MakeGarbageCollected<DOMTask>(
      isolate, callback, resolver, task_context);
  if (signal) {
    signal->AddAlgorithm(
        bindings::MakeGarbageCollected<AbortAlgorithm<DOMTask>>(task));
  }
  ++pending_tasks_;
  RunnerFor(priority, /*continuation=*/false)
      ->PostDelayedTask(FROM_HERE,
                        base::BindOnce(
                            [](cppgc::WeakPersistent<Scheduler> scheduler,
                               cppgc::Persistent<DOMTask> task) {
                              if (scheduler) {
                                --scheduler->pending_tasks_;
                              }
                              task->Invoke();
                            },
                            cppgc::WeakPersistent<Scheduler>(this),
                            cppgc::Persistent<DOMTask>(task)),
                        base::Milliseconds(options.delay));
  return resolver->GetPromise();
}

// DOMScheduler::yield().
v8::Local<v8::Promise> Scheduler::Yield(
    bindings::ExceptionState& exception_state) {
  if (!CheckAttached(exception_state)) {
    return {};
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return {};
  }
  SchedulerTaskContext* task_context = SchedulerTaskContext::Current(isolate);
  if (task_context && !task_context->CanPropagateTo(window_.Get())) {
    task_context = nullptr;
  }
  AbortSignal* const abort_source =
      task_context ? task_context->abort_source() : nullptr;
  if (abort_source && abort_source->Aborted()) {
    std::ignore = resolver->Reject(context, abort_source->Reason());
    return resolver->GetPromise();
  }
  const Priority priority =
      task_context ? task_context->priority() : Priority::kUserVisible;
  DOMTaskContinuation* const continuation =
      bindings::MakeGarbageCollected<DOMTaskContinuation>(isolate, resolver,
                                                          abort_source);
  if (abort_source) {
    abort_source->AddAlgorithm(
        bindings::MakeGarbageCollected<AbortAlgorithm<DOMTaskContinuation>>(
            continuation));
  }
  ++pending_tasks_;
  RunnerFor(priority, /*continuation=*/true)
      ->PostTask(FROM_HERE,
                 base::BindOnce(
                     [](cppgc::WeakPersistent<Scheduler> scheduler,
                        cppgc::Persistent<DOMTaskContinuation> continuation) {
                       if (scheduler) {
                         --scheduler->pending_tasks_;
                       }
                       continuation->Invoke();
                     },
                     cppgc::WeakPersistent<Scheduler>(this),
                     cppgc::Persistent<DOMTaskContinuation>(continuation)));
  return resolver->GetPromise();
}

}  // namespace mitto
