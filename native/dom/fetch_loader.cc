#include "mitto/dom/fetch_loader.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/strcat.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/dom/abort_signal.h"
#include "mitto/dom/body_stream_buffer.h"
#include "mitto/dom/buffering_bytes_consumer.h"
#include "mitto/dom/bytes_uploader.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/form_data_bytes_consumer.h"
#include "mitto/dom/headers.h"
#include "mitto/dom/pending_script.h"
#include "mitto/dom/readable_stream.h"
#include "mitto/dom/response.h"
#include "mitto/dom/subresource_integrity.h"
#include "mitto/dom/window_task.h"
#include "mitto/header_rules.h"
#include "url/gurl.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"

namespace mitto {
namespace {

// The Response of `type` that `head` starts, with `headers` and `body`.
Response* CreateResponse(Response::ResponseType type,
                         const FetchResponseHead& head,
                         Headers* headers,
                         BodyStreamBuffer* body) {
  // The getter of Response's url leaves out the fragment that the response
  // keeps. No script reads an opaque one's.
  Response* const response = Response::CreateFetched(
      type, head.status, head.status_text,
      base::UTF8ToUTF16(type == Response::ResponseType::kOpaque
                            ? head.url
                            : GURL(head.url).GetWithoutRef().spec()),
      head.redirected, headers, body);
  response->SetPadding(head.padding);
  return response;
}

// The names that Access-Control-Expose-Headers lists, in lower case, with
// "*" among them, which exposes every name only for a request without
// credentials, as Blink's ExtractCorsExposedHeaderNamesList() has it.
// Upstream: third_party/blink/renderer/platform/loader/cors/cors.cc
std::set<std::string> ExposedHeaderNames(const Headers& headers) {
  std::set<std::string> names;
  const std::optional<std::string> list =
      headers.GetValue("access-control-expose-headers");
  if (!list) {
    return names;
  }
  for (const std::string_view name : base::SplitStringPiece(
           *list, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (IsHttpToken(name) || name == "*") {
      names.insert(base::ToLowerASCII(name));
    }
  }
  return names;
}

// The abort algorithm of a fetch, which holds the loader weakly, as Blink's
// binds FetchLoaderBase::Abort().
class AbortFetchAlgorithm final : public AbortSignal::Algorithm {
 public:
  explicit AbortFetchAlgorithm(base::WeakPtr<FetchLoader> loader)
      : loader_(std::move(loader)) {}

  void Run() override {
    if (FetchLoader* const loader = loader_.get()) {
      loader->Abort();
    }
  }

 private:
  base::WeakPtr<FetchLoader> loader_;
};

// Blink's IntegrityVerifier: reads all of a response's body, checks it
// against the request's integrity metadata, and settles the fetch's promise,
// with a Response of the body that passed, or with "Failed to fetch" and the
// console's reasons. It holds itself until it settles, since the FetchLoader
// goes once the body has all come.
class IntegrityVerifier final
    : public cppgc::GarbageCollected<IntegrityVerifier>,
      public BytesConsumer::Client {
 public:
  // What the Response of a body that passes gets.
  struct Pending {
    Pending() = default;
    Pending(Pending&&) = default;
    Pending& operator=(Pending&&) = default;
    ~Pending() = default;

    Response::ResponseType type = Response::ResponseType::kBasic;
    FetchResponseHead head;
    std::u16string integrity;
    // The request's, which the console names.
    GURL url;
  };

  IntegrityVerifier(v8::Isolate* isolate,
                    v8::Local<v8::Context> context,
                    v8::Local<v8::Promise::Resolver> resolver,
                    Pending pending,
                    Headers* headers,
                    AbortSignal* signal,
                    BytesConsumer* body)
      : isolate_(isolate),
        context_(isolate, context),
        resolver_(isolate, resolver),
        pending_(std::move(pending)),
        headers_(headers),
        signal_(signal),
        body_(body),
        self_(this) {
    // As Blink's verifier goes with its loader, once the context does.
    if (ExecutionContext* const execution_context =
            ExecutionContext::From(isolate, context)) {
      execution_context_ = execution_context;
      context_destroyed_callback_ =
          execution_context->AddContextDestroyedCallback(base::BindOnce(
              [](cppgc::WeakPersistent<IntegrityVerifier> verifier) {
                if (verifier) {
                  verifier->ContextDestroyed();
                }
              },
              cppgc::WeakPersistent<IntegrityVerifier>(this)));
    }
  }

  // Starts reading, from a networking task, as Blink's verifier first hears
  // of a body from its data pipe.
  void Start() {
    if (done_) {
      return;
    }
    body_->SetClient(this);
    OnStateChange();
  }

  void OnStateChange() override {
    while (!done_) {
      base::span<const char> buffer;
      BytesConsumer::Result result = body_->BeginRead(buffer);
      if (result == BytesConsumer::Result::kOk) {
        buffer_.append(buffer.begin(), buffer.end());
        result = body_->EndRead(buffer.size());
      }
      switch (result) {
        case BytesConsumer::Result::kOk:
          break;
        case BytesConsumer::Result::kShouldWait:
          return;
        case BytesConsumer::Result::kDone:
          Check();
          return;
        case BytesConsumer::Result::kError:
          Fail("");
          return;
      }
    }
  }

  // The signal aborted: the promise rejects with its reason.
  void Abort() {
    if (done_) {
      return;
    }
    const v8::HandleScope handle_scope(isolate_);
    const v8::Local<v8::Context> context = context_.Get(isolate_);
    const v8::Context::Scope context_scope(context);
    std::ignore = resolver_.Get(isolate_)->Reject(context, signal_->Reason());
    body_->Cancel();
    Done();
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(execution_context_);
    visitor->Trace(headers_);
    visitor->Trace(signal_);
    visitor->Trace(body_);
    BytesConsumer::Client::Trace(visitor);
  }

 private:
  // A frame that goes fails its body first, as Blink's stops its loads before
  // its context goes, with nothing in the console.
  void ContextDestroyed() {
    context_destroyed_callback_.reset();
    if (done_) {
      return;
    }
    body_->Cancel();
    const v8::HandleScope handle_scope(isolate_);
    const v8::Local<v8::Context> context = context_.Get(isolate_);
    const v8::Context::Scope context_scope(context);
    Reject(context);
    Done();
  }

  void Reject(v8::Local<v8::Context> context) {
    std::ignore = resolver_.Get(isolate_)->Reject(
        context, v8::Exception::TypeError(v8::String::NewFromUtf8Literal(
                     isolate_, "Failed to fetch")));
  }

  void Check() {
    const v8::HandleScope handle_scope(isolate_);
    const v8::Local<v8::Context> context = context_.Get(isolate_);
    const v8::Context::Scope context_scope(context);
    std::vector<std::string> messages;
    const IntegrityMetadataSet metadata =
        ParseIntegrityAttribute(pending_.integrity, &messages);
    const std::string response_url = ElidedUrl(GURL(pending_.head.url));
    // SubresourceIntegrity::CheckSubresourceIntegrity() of a response type,
    // which it reads before whatever the metadata says.
    bool passed = false;
    if (pending_.type == Response::ResponseType::kBasic ||
        pending_.type == Response::ResponseType::kCors) {
      passed = CheckSubresourceIntegrity(metadata, buffer_, response_url,
                                         /*cors_same_origin=*/true,
                                         pending_.head.headers, messages);
    } else {
      messages.push_back(base::StrCat(
          {"Subresource Integrity: The resource '", response_url,
           "' has an integrity attribute, but the response is not eligible "
           "for integrity validation."}));
    }
    if (ExecutionContext* const execution_context =
            ExecutionContext::From(isolate_, context)) {
      for (std::string& message : messages) {
        execution_context->AddConsoleMessage(ConsoleLevel::kError,
                                             std::move(message));
      }
    }
    if (!passed) {
      Fail("SRI's integrity checks failed.");
      return;
    }
    // The body that passed, whole, as Blink's verifier gives it to the
    // Response.
    BodyStreamBuffer* const body = BodyStreamBuffer::Create(
        isolate_,
        bindings::MakeGarbageCollected<FormDataBytesConsumer>(
            std::move(buffer_)),
        signal_.Get());
    Response* const response =
        CreateResponse(pending_.type, pending_.head, headers_.Get(), body);
    const v8::Local<v8::Promise::Resolver> resolver = resolver_.Get(isolate_);
    Done();
    std::ignore =
        resolver->Resolve(context, bindings::ToV8(isolate_, response));
  }

  // Blink's PerformNetworkError(): "Failed to fetch", and why in the
  // console.
  void Fail(std::string_view issue_summary) {
    const v8::HandleScope handle_scope(isolate_);
    const v8::Local<v8::Context> context = context_.Get(isolate_);
    const v8::Context::Scope context_scope(context);
    if (ExecutionContext* const execution_context =
            ExecutionContext::From(isolate_, context)) {
      execution_context->AddConsoleMessage(
          ConsoleLevel::kError,
          base::StrCat({"Fetch API cannot load ", ElidedUrl(pending_.url), ". ",
                        issue_summary}));
    }
    Reject(context);
    Done();
  }

  void Done() {
    done_ = true;
    if (context_destroyed_callback_ && execution_context_) {
      execution_context_->RemoveContextDestroyedCallback(
          *context_destroyed_callback_);
    }
    context_destroyed_callback_.reset();
    resolver_.Reset();
    context_.Reset();
    self_.Clear();
  }

  const raw_ptr<v8::Isolate> isolate_;
  v8::Global<v8::Context> context_;
  v8::Global<v8::Promise::Resolver> resolver_;
  Pending pending_;
  const cppgc::Member<Headers> headers_;
  const cppgc::Member<AbortSignal> signal_;
  const cppgc::Member<BytesConsumer> body_;
  std::string buffer_;
  bool done_ = false;
  cppgc::WeakMember<ExecutionContext> execution_context_;
  std::optional<uint64_t> context_destroyed_callback_;
  // Itself, until it settles.
  cppgc::Persistent<IntegrityVerifier> self_;
};

class AbortIntegrityAlgorithm final : public AbortSignal::Algorithm {
 public:
  explicit AbortIntegrityAlgorithm(IntegrityVerifier* verifier)
      : verifier_(verifier) {}

  void Run() override {
    if (IntegrityVerifier* const verifier = verifier_.Get()) {
      verifier->Abort();
    }
  }

  void Trace(cppgc::Visitor* visitor) const override {
    AbortSignal::Algorithm::Trace(visitor);
    visitor->Trace(verifier_);
  }

 private:
  const cppgc::WeakMember<IntegrityVerifier> verifier_;
};

}  // namespace

FetchLoader::FetchLoader(v8::Isolate* isolate,
                         v8::Local<v8::Context> context,
                         v8::Local<v8::Promise::Resolver> resolver,
                         bool credentials,
                         AbortSignal* signal,
                         BytesConsumer* upload_body,
                         ReadableStream* upload_stream)
    : isolate_(isolate),
      context_(isolate, context),
      resolver_(isolate, resolver),
      credentials_(credentials),
      signal_(signal),
      upload_body_(upload_body),
      upload_stream_(upload_stream) {
  signal->AddAlgorithm(bindings::MakeGarbageCollected<AbortFetchAlgorithm>(
      weak_factory_.GetWeakPtr()));
}

void FetchLoader::SetModes(FetchRedirectMode redirect_mode,
                           std::u16string integrity,
                           GURL url) {
  redirect_mode_ = redirect_mode;
  integrity_ = std::move(integrity);
  url_ = std::move(url);
}

FetchLoader::~FetchLoader() {
  if (BodyLoaderBytesConsumer* const consumer = consumer_.Get()) {
    consumer->DetachLoader();
  }
  // What is left of an upload goes with the window or worker.
  if (uploader_) {
    uploader_->Detach();
  }
}

void FetchLoader::OnStarted(FetchController& controller) {
  controller_ = &controller;
  if (upload_body_) {
    const v8::HandleScope handle_scope(isolate_);
    uploader_ = bindings::MakeGarbageCollected<BytesUploader>(
        upload_body_.Get(), controller,
        ExecutionContext::From(isolate_, context_.Get(isolate_)));
    upload_body_.Clear();
  }
}

void FetchLoader::OnResponseStarted(const FetchResponseHead& head) {
  const v8::HandleScope handle_scope(isolate_);
  const v8::Local<v8::Context> context = context_.Get(isolate_);
  const v8::Context::Scope context_scope(context);
  // Upstream: third_party/blink/renderer/core/fetch/fetch_manager.cc
  // Every response has a body, an empty one for a HEAD request or a null body
  // status, whose bytes wait in the consumer until they are read. Unless the
  // response says no-store, Blink's BufferingBytesConsumer reads them 50 ms
  // after the body starts.
  //
  // ResourceResponse::AddHttpHeaderField() joins the values of a name's lines,
  // which the Cache API then stores as one.
  std::vector<std::pair<std::string, std::string>> lines;
  std::map<std::string, size_t> places;
  for (const std::string_view line :
       base::SplitStringPiece(head.headers, "\n", base::KEEP_WHITESPACE,
                              base::SPLIT_WANT_NONEMPTY)) {
    const size_t colon = line.find(": ");
    if (colon == std::string_view::npos) {
      continue;
    }
    const std::string_view name = line.substr(0, colon);
    const std::string_view value = line.substr(colon + 2);
    const auto [place, added] =
        places.emplace(base::ToLowerASCII(name), lines.size());
    if (added) {
      lines.emplace_back(name, value);
    } else {
      base::StrAppend(&lines[place->second].second, {", ", value});
    }
  }
  Headers* const all = Headers::Create(Headers::Guard::kNone);
  for (auto& [name, value] : lines) {
    all->AppendLine(std::move(name), std::move(value));
  }
  // A redirect that the Session did not follow, for redirect mode error or
  // manual, as Blink's WillFollowRedirect() takes it: a network error with
  // nothing in the console, or an opaque-redirect filtered response.
  const bool redirect_status = head.status == 301 || head.status == 302 ||
                               head.status == 303 || head.status == 307 ||
                               head.status == 308;
  // The Session stops only at a redirect with a Location, but in mode manual
  // a redirect's status alone makes the response opaque-redirect, as in
  // Blink's DidReceiveResponse().
  const bool is_redirect =
      redirect_status && redirect_mode_ == FetchRedirectMode::kManual;
  if (redirect_status && redirect_mode_ == FetchRedirectMode::kError &&
      HasLocation(head.headers)) {
    std::ignore = resolver_.Get(isolate_)->Reject(
        context, v8::Exception::TypeError(v8::String::NewFromUtf8Literal(
                     isolate_, "Failed to fetch")));
    resolver_.Reset();
    // Destroys this.
    Cancel();
    return;
  }
  auto* const consumer =
      bindings::MakeGarbageCollected<BodyLoaderBytesConsumer>(
          this, ExecutionContext::From(isolate_, context));
  consumer_ = consumer;
  const std::optional<std::string> cache_control =
      all->GetValue("cache-control");
  BytesConsumer* body_consumer = consumer;
  // An integrity check reads the body as it comes.
  if (integrity_.empty() &&
      (!cache_control || !CacheControlContainsNoStore(*cache_control))) {
    body_consumer = BufferingBytesConsumer::CreateWithDelay(consumer);
  } else if (ExecutionContext* const execution_context =
                 ExecutionContext::From(isolate_, context)) {
    // ThreadableLoader::ResponseReceived(): a script's fetch() whose response
    // says no-store keeps its context's page out of the back/forward cache,
    // one that a link preloaded for it included.
    execution_context->DidReceiveNoStoreForScriptRequest();
  }
  // The Response of `type`, with `headers`, resolves the promise now, or once
  // an integrity check has the whole body, as Blink's IntegrityVerifier
  // reads it.
  const auto deliver = [&](Response::ResponseType type, Headers* headers) {
    if (!integrity_.empty()) {
      IntegrityVerifier::Pending pending;
      pending.type = type;
      pending.head = head;
      pending.integrity = integrity_;
      pending.url = url_;
      auto* const verifier = bindings::MakeGarbageCollected<IntegrityVerifier>(
          isolate_, context, resolver_.Get(isolate_), std::move(pending),
          headers, signal_.Get(), consumer);
      resolver_.Reset();
      signal_->AddAlgorithm(
          bindings::MakeGarbageCollected<AbortIntegrityAlgorithm>(verifier));
      PostTaskInContext(ExecutionContext::From(isolate_, context),
                        TaskType::kNetworking,
                        base::BindOnce(
                            [](cppgc::Persistent<IntegrityVerifier> verifier) {
                              verifier->Start();
                            },
                            cppgc::Persistent<IntegrityVerifier>(verifier)));
      return;
    }
    BodyStreamBuffer* const body =
        BodyStreamBuffer::Create(isolate_, body_consumer, signal_.Get());
    Response* const response = CreateResponse(type, head, headers, body);
    std::ignore = resolver_.Get(isolate_)->Resolve(
        context, bindings::ToV8(isolate_, response));
    resolver_.Reset();
  };
  if (head.opaque || is_redirect) {
    // Upstream: third_party/blink/renderer/core/fetch/fetch_response_data.cc
    // An opaque or opaque-redirect filtered response, whose body no script
    // reads, over the response with every line that the library sends.
    all->set_guard(Headers::Guard::kImmutable);
    deliver(is_redirect ? Response::ResponseType::kOpaqueRedirect
                        : Response::ResponseType::kOpaque,
            all);
    return;
  }
  const bool cors = head.cors;
  // What a script reads of the response's header lines: no Set-Cookie, and of
  // a cross-origin response only the safelisted names and the exposed ones,
  // as Blink's FetchResponseData filters them.
  Headers* const headers = Headers::Create(Headers::Guard::kNone);
  const std::set<std::string> exposed =
      cors ? ExposedHeaderNames(*all) : std::set<std::string>();
  // With credentials, "*" is a name like any other.
  const bool expose_all = !credentials_ && exposed.contains("*");
  for (const auto& [name, value] : all->list()) {
    if (IsForbiddenResponseHeaderName(name)) {
      continue;
    }
    if (cors && !IsCorsSafelistedResponseHeaderName(name) &&
        !exposed.contains(base::ToLowerASCII(name)) && !expose_all) {
      continue;
    }
    headers->AppendLine(name, value);
  }
  headers->set_guard(Headers::Guard::kImmutable);
  deliver(cors ? Response::ResponseType::kCors : Response::ResponseType::kBasic,
          headers);
}

void FetchLoader::OnResponseBody(std::string bytes) {
  BodyLoaderBytesConsumer* const consumer = consumer_.Get();
  if (!consumer) {
    // What read the body went, and the request is about to be cancelled.
    return;
  }
  const v8::HandleScope handle_scope(isolate_);
  const v8::Context::Scope context_scope(context_.Get(isolate_));
  consumer->AppendBody(bytes);
}

void FetchLoader::OnResponseEnded(int net_error) {
  const v8::HandleScope handle_scope(isolate_);
  const v8::Local<v8::Context> context = context_.Get(isolate_);
  const v8::Context::Scope context_scope(context);
  if (uploader_) {
    uploader_->OnRequestGone();
    uploader_.Clear();
  }
  // A body that an integrity check reads still ends through its consumer.
  if (!resolver_.IsEmpty()) {
    // Upstream: third_party/blink/renderer/core/fetch/fetch_manager.cc
    std::ignore = resolver_.Get(isolate_)->Reject(
        context, v8::Exception::TypeError(v8::String::NewFromUtf8Literal(
                     isolate_, "Failed to fetch")));
    resolver_.Reset();
    return;
  }
  if (BodyLoaderBytesConsumer* const consumer = consumer_.Get()) {
    consumer->DetachLoader();
    consumer_.Clear();
    if (net_error == 0) {
      consumer->SignalComplete();
    } else {
      consumer->SignalError();
    }
  }
}

void FetchLoader::OnBodyRoom() {
  if (!uploader_) {
    return;
  }
  const v8::HandleScope handle_scope(isolate_);
  const v8::Context::Scope context_scope(context_.Get(isolate_));
  uploader_->OnRoom();
}

void FetchLoader::BodyRequested() {}

void FetchLoader::BodyTaken(size_t bytes) {
  controller_->TookBody(bytes);
}

void FetchLoader::BodyDropped() {
  EventLoop::Get()
      ->TaskRunner(TaskType::kNetworking)
      ->PostTask(FROM_HERE, base::BindOnce(&FetchLoader::Cancel,
                                           weak_factory_.GetWeakPtr()));
}

void FetchLoader::Abort() {
  const v8::HandleScope handle_scope(isolate_);
  const v8::Local<v8::Context> context = context_.Get(isolate_);
  const v8::Context::Scope context_scope(context);
  // The Fetch Standard's "abort a fetch": the promise rejects with the
  // reason, and the Response errors its own body.
  if (!resolver_.IsEmpty()) {
    std::ignore = resolver_.Get(isolate_)->Reject(context, signal_->Reason());
    resolver_.Reset();
  }
  // "If request's body is non-null and is readable, then cancel request's
  // body with error", once its upload has stopped without a word to the
  // stream, so that the cancel gives the stream's source the reason, even
  // while a chunk waits for room, and the end that it gives the upload goes
  // nowhere.
  if (uploader_) {
    uploader_->Detach();
    uploader_.Clear();
  }
  // The request goes first, which destroys this, so that the stream's
  // cancel, whose source may run a script that lets the context go, finds
  // nothing of this to touch.
  const cppgc::Persistent<ReadableStream> stream = upload_stream_.Get();
  upload_stream_.Clear();
  const v8::Local<v8::Value> reason = signal_->Reason();
  v8::Isolate* const isolate = isolate_;
  Cancel();
  if (stream) {
    std::ignore = ReadableStream::Cancel(isolate, stream.Get(), reason);
  }
}

void FetchLoader::Cancel() {
  if (uploader_) {
    const v8::HandleScope handle_scope(isolate_);
    const v8::Context::Scope context_scope(context_.Get(isolate_));
    uploader_->OnRequestGone();
    uploader_.Clear();
  }
  // A body that has not all come fails, unless what reads it has gone.
  if (BodyLoaderBytesConsumer* const consumer = consumer_.Get()) {
    consumer->DetachLoader();
    consumer->SignalErrorLater();
    consumer_.Clear();
  }
  // Destroys this.
  controller_->Cancel();
}

}  // namespace mitto
