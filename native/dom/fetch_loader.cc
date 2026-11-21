#include "mitto/dom/fetch_loader.h"

#include <set>
#include <tuple>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
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
#include "mitto/dom/headers.h"
#include "mitto/dom/readable_stream.h"
#include "mitto/dom/response.h"
#include "mitto/header_rules.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"

namespace mitto {
namespace {

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

// Whether a response's Cache-Control value, `value`, has the directive
// no-store, as Blink's ParseCacheControlDirectives() finds it, with its
// separators of RFC 2616.
// Upstream: third_party/blink/renderer/platform/network/http_parsers.cc
bool CacheControlContainsNoStore(std::string_view raw_value) {
  constexpr std::string_view kSeparators = "()<>@,;:\\\"/[]?={} \t";
  std::string value;
  for (const char c : raw_value) {
    if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F) {
      value.push_back(c);
    }
  }
  const auto strip = [](std::string_view text) {
    return base::TrimString(text, " ", base::TRIM_ALL);
  };
  const auto trim_to_separator = [&](std::string_view text) {
    return text.substr(0, text.find_first_of(kSeparators));
  };
  bool no_store = false;
  const auto add = [&](std::string_view directive) {
    if (base::EqualsCaseInsensitiveASCII(directive, "no-store")) {
      no_store = true;
    }
  };
  const std::string_view header = value;
  const size_t max = header.size();
  for (size_t pos = 0; pos < max;) {
    const size_t next_comma = header.find(',', pos);
    const size_t next_equal = header.find('=', pos);
    if (next_equal != std::string_view::npos &&
        (next_equal < next_comma || next_comma == std::string_view::npos)) {
      // A directive with a value.
      add(trim_to_separator(strip(header.substr(pos, next_equal - pos))));
      pos = next_equal + 1;
      const std::string_view rest = strip(header.substr(pos));
      if (rest.starts_with('"')) {
        const size_t next_quote = rest.find('"', 1);
        if (next_quote == std::string_view::npos) {
          return no_store;
        }
        pos = header.find('"', pos) + next_quote + 1;
        const size_t comma = header.find(',', pos);
        if (comma == std::string_view::npos) {
          return no_store;
        }
        pos = comma + 1;
      } else {
        if (rest.find(',') == std::string_view::npos) {
          return no_store;
        }
        pos = header.find(',', pos) + 1;
      }
    } else if (next_comma != std::string_view::npos &&
               (next_comma < next_equal ||
                next_equal == std::string_view::npos)) {
      add(trim_to_separator(strip(header.substr(pos, next_comma - pos))));
      pos = next_comma + 1;
    } else {
      add(trim_to_separator(strip(header.substr(pos))));
      return no_store;
    }
  }
  return no_store;
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
  Headers* const all = Headers::Create(Headers::Guard::kNone);
  for (const std::string_view line :
       base::SplitStringPiece(head.headers, "\n", base::KEEP_WHITESPACE,
                              base::SPLIT_WANT_NONEMPTY)) {
    const size_t colon = line.find(": ");
    if (colon != std::string_view::npos) {
      all->AppendLine(std::string(line.substr(0, colon)),
                      std::string(line.substr(colon + 2)));
    }
  }
  auto* const consumer =
      bindings::MakeGarbageCollected<BodyLoaderBytesConsumer>(
          this, ExecutionContext::From(isolate_, context));
  consumer_ = consumer;
  const std::optional<std::string> cache_control =
      all->GetValue("cache-control");
  BytesConsumer* body_consumer = consumer;
  if (!cache_control || !CacheControlContainsNoStore(*cache_control)) {
    body_consumer = BufferingBytesConsumer::CreateWithDelay(consumer);
  }
  BodyStreamBuffer* const body =
      BodyStreamBuffer::Create(isolate_, body_consumer, signal_.Get());
  if (head.opaque) {
    // Upstream: third_party/blink/renderer/core/fetch/fetch_response_data.cc
    // An opaque filtered response, whose body no script reads.
    Headers* const headers = Headers::Create(Headers::Guard::kImmutable);
    Response* const response = Response::CreateFetched(
        Response::ResponseType::kOpaque, /*status=*/0, /*status_text=*/"",
        /*url=*/u"", /*redirected=*/false, headers, body);
    std::ignore = resolver_.Get(isolate_)->Resolve(
        context, bindings::ToV8(isolate_, response));
    resolver_.Reset();
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
  Response* const response = Response::CreateFetched(
      cors ? Response::ResponseType::kCors : Response::ResponseType::kBasic,
      head.status, head.status_text, base::UTF8ToUTF16(head.url),
      head.redirected, headers, body);
  std::ignore = resolver_.Get(isolate_)->Resolve(
      context, bindings::ToV8(isolate_, response));
  resolver_.Reset();
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
  if (ReadableStream* const stream = upload_stream_.Get()) {
    upload_stream_.Clear();
    std::ignore = ReadableStream::Cancel(isolate_, stream, signal_->Reason());
  }
  Cancel();
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
