#include "mitto/dom/execution_context.h"

#include <stdint.h>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "base/base64.h"
#include "base/check.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_window.h"
#include "mitto/bindings/v8_worker_global_scope.h"
#include "mitto/dom/abort_signal.h"
#include "mitto/dom/body_stream_buffer.h"
#include "mitto/dom/document.h"
#include "mitto/dom/dom_timer.h"
#include "mitto/dom/fetch_loader.h"
#include "mitto/dom/fetcher.h"
#include "mitto/dom/headers.h"
#include "mitto/dom/local_scheme_fetch.h"
#include "mitto/dom/preloaded_fetch.h"
#include "mitto/dom/readable_stream_bytes_consumer.h"
#include "mitto/dom/request.h"
#include "mitto/dom/resource.h"
#include "mitto/dom/resource_fetcher.h"
#include "mitto/dom/window.h"
#include "mitto/dom/worker_global_scope.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-promise.h"

namespace mitto {

namespace {

// The string's code units as bytes, or nullopt if one is beyond Latin-1.
std::optional<std::string> ToLatin1(const std::u16string& text) {
  std::string bytes;
  bytes.reserve(text.size());
  for (const char16_t c : text) {
    if (c > 0xFF) {
      return std::nullopt;
    }
    bytes.push_back(static_cast<char>(c));
  }
  return bytes;
}

}  // namespace

// static
ExecutionContext& ExecutionContext::Current() {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  ExecutionContext* const context = From(isolate, isolate->GetCurrentContext());
  CHECK(context);
  return *context;
}

// static
ExecutionContext* ExecutionContext::From(v8::Isolate* isolate,
                                         v8::Local<v8::Context> context) {
  if (Window* const window =
          bindings::V8Window::ToWrappable(isolate, context->Global())) {
    return window;
  }
  return bindings::V8WorkerGlobalScope::ToWrappable(isolate, context->Global());
}

ExecutionContext::ExecutionContext() = default;
ExecutionContext::~ExecutionContext() = default;

ExecutionContext::PendingException::PendingException(
    v8::Global<v8::Value> exception,
    std::u16string text)
    : exception(std::move(exception)), text(std::move(text)) {}
ExecutionContext::PendingException::PendingException(PendingException&&) =
    default;
ExecutionContext::PendingException&
ExecutionContext::PendingException::operator=(PendingException&&) = default;
ExecutionContext::PendingException::~PendingException() = default;

ExecutionContext::ErrorEventState::ErrorEventState() = default;
ExecutionContext::ErrorEventState::~ErrorEventState() = default;

bool ExecutionContext::IsWindow() const {
  return false;
}

void ExecutionContext::SetStart(const PageStart& start,
                                const PageConnection& connection) {
  start_ = start;
  connection_ = connection;
}

void ExecutionContext::ReportError(v8::Local<v8::Value> e) {
  // WindowOrWorkerGlobalScope::reportError():
  // V8ScriptRunner::ReportException(), with V8's message of `e`, which says
  // where an error was made, and else where reportError() was called.
  bindings::ReportException(v8::Isolate::GetCurrent(), e);
}

void ExecutionContext::AddConsoleMessage(ConsoleLevel level, std::string text) {
  if (console_sink_) {
    console_sink_.Run(level, std::move(text));
  }
}

int32_t ExecutionContext::SetTimeout(
    v8::Local<v8::Function> handler,
    int32_t timeout,
    const std::vector<v8::Local<v8::Value>>& arguments) {
  return timers_ ? timers_->SetTimeout(handler, timeout, arguments) : 0;
}

int32_t ExecutionContext::SetTimeout(
    const std::u16string& handler,
    int32_t timeout,
    const std::vector<v8::Local<v8::Value>>& arguments,
    bindings::ExceptionState& exception_state) {
  // As Chrome, a string handler gets none of the arguments.
  return timers_ ? timers_->SetTimeout(handler, timeout) : 0;
}

void ExecutionContext::ClearTimeout(int32_t handle) {
  if (timers_) {
    timers_->Clear(handle);
  }
}

int32_t ExecutionContext::SetInterval(
    v8::Local<v8::Function> handler,
    int32_t timeout,
    const std::vector<v8::Local<v8::Value>>& arguments) {
  return timers_ ? timers_->SetInterval(handler, timeout, arguments) : 0;
}

int32_t ExecutionContext::SetInterval(
    const std::u16string& handler,
    int32_t timeout,
    const std::vector<v8::Local<v8::Value>>& arguments,
    bindings::ExceptionState& exception_state) {
  return timers_ ? timers_->SetInterval(handler, timeout) : 0;
}

void ExecutionContext::ClearInterval(int32_t handle) {
  if (timers_) {
    timers_->Clear(handle);
  }
}

void ExecutionContext::SetTimers(DOMTimerCoordinator* timers) {
  timers_ = timers;
  if (timers) {
    return;
  }
  // The context goes, and with it the signals' timers and what else goes
  // with it.
  std::map<uint64_t, base::OnceClosure> signal_timers;
  signal_timers.swap(signal_timers_);
  for (auto& [id, cancel] : signal_timers) {
    std::move(cancel).Run();
  }
  std::map<uint64_t, base::OnceClosure> callbacks;
  callbacks.swap(context_destroyed_callbacks_);
  for (auto& [id, callback] : callbacks) {
    std::move(callback).Run();
  }
}

uint64_t ExecutionContext::AddContextDestroyedCallback(
    base::OnceClosure callback) {
  const uint64_t id = ++next_context_destroyed_callback_;
  context_destroyed_callbacks_.emplace(id, std::move(callback));
  return id;
}

void ExecutionContext::RemoveContextDestroyedCallback(uint64_t id) {
  context_destroyed_callbacks_.erase(id);
}

uint64_t ExecutionContext::AddSignalTimer(base::OnceClosure cancel) {
  const uint64_t id = ++next_signal_timer_;
  signal_timers_.emplace(id, std::move(cancel));
  return id;
}

void ExecutionContext::RemoveSignalTimer(uint64_t id) {
  signal_timers_.erase(id);
}

size_t ExecutionContext::TimerCount() const {
  return (timers_ ? timers_->size() : 0) + signal_timers_.size();
}

void ExecutionContext::QueueMicrotask(v8::Local<v8::Function> callback) {
  // Blink queues a microtask of its own, which calls the callback and reports
  // what it throws with where it was thrown, where V8's own microtask of a
  // function would report where an error was made.
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  isolate->EnqueueMicrotask(&RunQueuedMicrotask,
                            new v8::Global<v8::Function>(isolate, callback));
}

// static
void ExecutionContext::RunQueuedMicrotask(void* data) {
  // V8VoidFunction::InvokeAndReportException(), with undefined for `this`.
  const std::unique_ptr<v8::Global<v8::Function>> held(
      static_cast<v8::Global<v8::Function>*>(data));
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::HandleScope handle_scope(isolate);
  const v8::Local<v8::Function> callback = held->Get(isolate);
  v8::Local<v8::Context> context;
  if (!callback->GetCreationContext(isolate).ToLocal(&context)) {
    return;
  }
  const v8::Context::Scope context_scope(context);
  // Not once the context has gone.
  ExecutionContext* const execution_context = From(isolate, context);
  if (!execution_context || !execution_context->HasContext()) {
    return;
  }
  const v8::TryCatch try_catch(isolate);
  if (callback->Call(context, v8::Undefined(isolate), 0, nullptr).IsEmpty() &&
      try_catch.HasCaught() && try_catch.CanContinue()) {
    bindings::ReportException(isolate, try_catch.Exception(),
                              try_catch.Message());
  }
}

// Upstream: third_party/blink/renderer/core/fetch/global_fetch.cc
// Upstream: third_party/blink/renderer/core/fetch/fetch_manager.cc
v8::Local<v8::Promise> ExecutionContext::Fetch(
    const bindings::RequestOrUSVString& input,
    const bindings::RequestInit& init,
    bindings::ExceptionState& exception_state) {
  Request* const request = Request::Create(input, init, exception_state);
  if (!request) {
    return v8::Local<v8::Promise>();
  }
  // The request's body goes to the fetch, and the Request keeps one that is
  // used, as Blink's PassRequestData() leaves it.
  BodyStreamBuffer* const body = request->PassBody(exception_state);
  if (exception_state.HadException()) {
    return v8::Local<v8::Promise>();
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return v8::Local<v8::Promise>();
  }
  // A request whose signal has aborted rejects with its reason, before any
  // check of where it goes.
  AbortSignal* const signal = request->Signal();
  if (signal->Aborted()) {
    std::ignore = resolver->Reject(context, signal->Reason());
    return resolver->GetPromise();
  }
  // A data: or a blob: URL, which Chrome answers without its network service,
  // as Blink's FetchManager::Loader::PerformSchemeFetch() fetches it.
  const GURL& url = request->url();
  if (url.SchemeIs(url::kDataScheme) || url.SchemeIs(url::kBlobScheme)) {
    const bool readable = url.SchemeIs(url::kDataScheme) ||
                          request->blob_url_origin()
                              .value_or(url::Origin::Create(url))
                              .IsSameOriginWith(GetSecurityOrigin());
    // FetchLoaderBase::Start(): a blob: URL of another origin is a network
    // error at once in mode same-origin, and in mode cors, whose scheme does
    // not support the Fetch API, so an abort after fetch() returns changes
    // nothing.
    if (!readable && request->mode() != FetchMode::kNoCors) {
      std::ignore = resolver->Reject(
          context, v8::Exception::TypeError(v8::String::NewFromUtf8Literal(
                       isolate, "Failed to fetch")));
      return resolver->GetPromise();
    }
    // A stream body goes nowhere, but an abort cancels it, as the Fetch
    // Standard's abort steps cancel a request's body that is readable.
    auto loader = std::make_unique<FetchLoader>(
        isolate, context, resolver, /*credentials=*/false, signal,
        /*upload_body=*/nullptr,
        body && body->IsMadeFromReadableStream() ? body->Stream() : nullptr);
    LocalSchemeFetch::Answer answer;
    if (url.SchemeIs(url::kDataScheme)) {
      // A data: URL's content can always be read, whatever the mode.
      answer = LocalSchemeFetch::AnswerDataUrl(url, request->Method());
    } else if (!readable) {
      // A blob: URL of another origin in mode no-cors: the network error
      // that the browser's partition check gives, since another origin's
      // blob is of another storage key.
      answer = LocalSchemeFetch::AnswerRefusedBlobUrl();
    } else {
      answer = LocalSchemeFetch::AnswerBlobUrl(
          url, request->blob_url_handle(), request->Method(),
          request->GetHeaders()->GetValue("range"));
    }
    LocalSchemeFetch::Start(std::move(answer), std::move(loader), *this);
    return resolver->GetPromise();
  }
  // The document's origin, or the worker's URL's.
  const url::Origin document_origin =
      url::Origin::Create(GURL(base::UTF16ToUTF8(Url())));
  // A cross-origin request of mode same-origin fails before anything goes, as
  // CorsURLLoader fails it. One that needs a CORS preflight goes to the
  // Session, which makes the preflight first.
  const bool cross_origin = !document_origin.IsSameOriginWith(request->url());
  Fetcher* const fetcher = GetFetcher();
  if (!fetcher || (cross_origin && request->mode() == FetchMode::kSameOrigin)) {
    std::ignore = resolver->Reject(
        context, v8::Exception::TypeError(v8::String::NewFromUtf8Literal(
                     isolate, "Failed to fetch")));
    return resolver->GetPromise();
  }
  FetchRequestData data;
  // A body of bytes that a script gave whole goes as they are. A stream's
  // goes as the Session reads it, through a reader of its own that locks it
  // now, as Blink's DrainAsChunkedDataPipeGetter() does: a chunked upload,
  // which goes only over HTTP/2 or HTTP/3.
  BytesConsumer* upload_body = nullptr;
  if (body && request->Method() != "GET" && request->Method() != "HEAD") {
    data.body = body->DrainAsBytes(exception_state);
    if (exception_state.HadException()) {
      return v8::Local<v8::Promise>();
    }
    if (!data.body && body->IsMadeFromReadableStream()) {
      upload_body = bindings::MakeGarbageCollected<ReadableStreamBytesConsumer>(
          isolate, body->Stream());
      data.streams_body = true;
    }
  }
  data.method = request->Method();
  data.url = request->url().spec();
  data.headers = request->GetHeaders()->ToLines();
  // A fetch's own are mode cors and credentials same-origin.
  if (request->mode() != FetchMode::kCors) {
    data.mode = request->mode();
  }
  if (request->credentials() != FetchCredentialsMode::kSameOrigin) {
    data.credentials = request->credentials();
  }
  data.referrer = request->referrer();
  // A request whose policy is the empty string goes with the document's. A
  // worker's would go with the policy of its script's response, which it does
  // not have: a Known Gap.
  data.referrer_policy =
      request->referrer_policy() == FetchReferrerPolicy::kDefault && IsWindow()
          ? static_cast<Window*>(this)->GetDocument().GetReferrerPolicy()
          : request->referrer_policy();
  data.priority = ComputeFetchPriority(/*lazy=*/false, request->priority(),
                                       /*render_blocking=*/false);
  data.keepalive = request->keepalive();
  auto loader = std::make_unique<FetchLoader>(
      isolate, context, resolver,
      /*credentials=*/request->credentials() == FetchCredentialsMode::kInclude,
      signal, upload_body, upload_body ? body->Stream() : nullptr);
  // A window's GET takes what a link preloaded as fetch, as Blink's
  // ThreadableLoader asks the document's ResourceFetcher for a resource of
  // type kRaw, with what Resource::CanReuse() wants of the request beside its
  // mode and credentials mode: no keepalive, and no header of a conditional
  // request.
  WindowTasks* const tasks = GetTasks();
  const Headers& headers = *request->GetHeaders();
  if (IsWindow() && tasks && data.method == "GET" && !data.keepalive &&
      std::ranges::none_of(
          std::to_array<std::string_view>({"if-match", "if-modified-since",
                                           "if-none-match", "if-range",
                                           "if-unmodified-since"}),
          [&headers](std::string_view name) {
            return headers.GetValue(name).has_value();
          })) {
    FetchParameters params;
    params.url = request->url();
    params.type = ResourceType::kRaw;
    params.mode = request->mode();
    params.credentials = request->credentials();
    if (Resource* const preload =
            static_cast<Window*>(this)->GetDocument().Fetcher().MatchPreload(
                params)) {
      PreloadedFetch::Start(*preload, std::move(loader), *this);
      return resolver->GetPromise();
    }
  }
  fetcher->Fetch(std::move(data), std::move(loader));
  return resolver->GetPromise();
}

// Upstream: third_party/blink/renderer/platform/wtf/text/base64.cc
std::u16string ExecutionContext::Btoa(
    const std::u16string& data,
    bindings::ExceptionState& exception_state) {
  const std::optional<std::string> bytes = ToLatin1(data);
  if (!bytes) {
    exception_state.ThrowDOMException(
        "InvalidCharacterError",
        "The string to be encoded contains characters outside of the Latin1 "
        "range.");
    return {};
  }
  return base::ASCIIToUTF16(base::Base64Encode(*bytes));
}

// The Infra Standard's forgiving-base64 decode, as Blink's Base64Decode()
// runs it: modp_b64's forgiving policy, and again without ASCII whitespace if
// that fails, which base::Base64Decode() does the same way.
std::u16string ExecutionContext::Atob(
    const std::u16string& data,
    bindings::ExceptionState& exception_state) {
  const std::optional<std::string> bytes = ToLatin1(data);
  if (!bytes) {
    exception_state.ThrowDOMException(
        "InvalidCharacterError",
        "The string to be decoded contains characters outside of the Latin1 "
        "range.");
    return {};
  }
  std::string decoded;
  if (!base::Base64Decode(*bytes, &decoded,
                          base::Base64DecodePolicy::kForgiving)) {
    exception_state.ThrowDOMException(
        "InvalidCharacterError",
        "The string to be decoded is not correctly encoded.");
    return {};
  }
  // Each byte is a code unit, as Blink's String of the bytes is Latin-1.
  std::u16string text;
  text.reserve(decoded.size());
  for (const char byte : decoded) {
    text.push_back(static_cast<uint8_t>(byte));
  }
  return text;
}

}  // namespace mitto
