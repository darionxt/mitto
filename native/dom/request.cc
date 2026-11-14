#include "mitto/dom/request.h"

#include <variant>
#include <vector>

#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_messages.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_request.h"
#include "mitto/dom/abort_signal.h"
#include "mitto/dom/document.h"
#include "mitto/dom/headers.h"
#include "mitto/dom/window.h"
#include "mitto/header_rules.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-isolate.h"

namespace mitto {
namespace {

// The Fetch Standard's normalize: the methods that it writes in upper case
// however they are given.
std::string NormalizeMethod(const std::string& method) {
  for (const std::string_view normalized :
       {"DELETE", "GET", "HEAD", "OPTIONS", "POST", "PUT"}) {
    if (base::EqualsCaseInsensitiveASCII(method, normalized)) {
      return std::string(normalized);
    }
  }
  return method;
}

// Whether `init` sets a member whose behaviour the Runtime lacks, to a value
// other than its default.
bool HasMissingMember(const bindings::RequestInit& init) {
  return (init.referrer && *init.referrer != u"about:client") ||
         (init.referrer_policy && !init.referrer_policy->empty()) ||
         (init.mode && *init.mode == u"no-cors") ||
         (init.credentials && *init.credentials == u"omit") ||
         (init.cache && *init.cache != u"default") ||
         (init.redirect && *init.redirect != u"follow") ||
         (init.integrity && !init.integrity->empty()) ||
         (init.keepalive && *init.keepalive) ||
         (init.priority && *init.priority != u"auto") ||
         init.ad_auction_headers.has_value() ||
         init.target_address_space.has_value() ||
         init.private_token.has_value() || init.retry_options.has_value();
}

}  // namespace

std::optional<std::string> ExtractBody(
    v8::Isolate* isolate,
    v8::Local<v8::Value> value,
    std::optional<std::string>* content_type) {
  if (value->IsArrayBuffer()) {
    const std::shared_ptr<v8::BackingStore> store =
        value.As<v8::ArrayBuffer>()->GetBackingStore();
    return std::string(static_cast<const char*>(store->Data()),
                       store->ByteLength());
  }
  if (value->IsArrayBufferView()) {
    const v8::Local<v8::ArrayBufferView> view = value.As<v8::ArrayBufferView>();
    std::string bytes(view->ByteLength(), '\0');
    view->CopyContents(bytes.data(), bytes.size());
    return bytes;
  }
  std::u16string text;
  if (!bindings::FromV8USVString(isolate, value, &text)) {
    return std::nullopt;
  }
  *content_type = "text/plain;charset=UTF-8";
  return base::UTF16ToUTF8(text);
}

// static
Request* Request::Create(const bindings::RequestOrUSVString& input,
                         const bindings::RequestInit& init,
                         bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  Request* const request = bindings::MakeGarbageCollected<Request>();
  Request* const input_request = std::holds_alternative<Request*>(input)
                                     ? std::get<Request*>(input)
                                     : nullptr;
  // The signal that the new one's follows: the input's, or init's, which may
  // be null.
  AbortSignal* signal = nullptr;
  if (input_request) {
    request->url_ = input_request->url_;
    request->method_ = input_request->method_;
    request->mode_ = input_request->mode_;
    request->body_ = input_request->body_;
    signal = input_request->signal_.Get();
  } else {
    // The API base URL: the document's base URL, against which the input
    // parses as the URL Standard has it, with its query in UTF-8 whatever the
    // document's encoding, as Blink's KURL(base_url, input) does.
    const std::u16string& input_string = std::get<std::u16string>(input);
    const GURL parsed =
        Window::Current().GetDocument().BaseUrl().Resolve(input_string);
    if (!parsed.is_valid()) {
      exception_state.ThrowTypeError(base::StrCat(
          {"Failed to parse URL from ", base::UTF16ToUTF8(input_string)}));
      return nullptr;
    }
    if (parsed.has_username() || parsed.has_password()) {
      exception_state.ThrowTypeError(
          base::StrCat({"Request cannot be constructed from a URL that "
                        "includes credentials: ",
                        base::UTF16ToUTF8(input_string)}));
      return nullptr;
    }
    request->url_ = parsed;
  }
  if (init.mode && *init.mode == u"navigate") {
    exception_state.ThrowTypeError(
        "Cannot construct a Request with a RequestInit whose mode member is "
        "set as 'navigate'.");
    return nullptr;
  }
  if (HasMissingMember(init)) {
    exception_state.ThrowTypeError(
        bindings::ExceptionMessages::NotImplemented());
    return nullptr;
  }
  if (init.mode && *init.mode == u"same-origin") {
    request->mode_ = Mode::kSameOrigin;
  } else if (init.mode) {
    request->mode_ = Mode::kCors;
  }
  if (init.method) {
    const std::string& method = *init.method;
    if (!IsHttpToken(method)) {
      exception_state.ThrowTypeError(
          base::StrCat({"'", method, "' is not a valid HTTP method."}));
      return nullptr;
    }
    if (IsForbiddenMethod(method)) {
      exception_state.ThrowTypeError(
          base::StrCat({"'", method, "' HTTP method is unsupported."}));
      return nullptr;
    }
    request->method_ = NormalizeMethod(method);
  }
  if (init.signal) {
    signal = *init.signal;
  }
  // A new dependent signal that follows it, as Blink makes before the
  // Request.
  std::vector<AbortSignal*> signals;
  if (signal) {
    signals.push_back(signal);
  }
  request->signal_ = AbortSignal::CreateComposite(signals);
  request->headers_ = Headers::Create(Headers::Guard::kRequest);
  if (init.headers) {
    request->headers_->FillWith(*init.headers, exception_state);
  } else if (input_request) {
    for (const auto& [name, value] : input_request->headers_->list()) {
      request->headers_->Append(name, value, exception_state);
    }
  }
  if (exception_state.HadException()) {
    return nullptr;
  }
  const bool has_init_body = init.body && !(*init.body)->IsNullOrUndefined();
  if ((has_init_body || request->body_) &&
      (request->method_ == "GET" || request->method_ == "HEAD")) {
    exception_state.ThrowTypeError(
        "Request with GET/HEAD method cannot have body.");
    return nullptr;
  }
  if (has_init_body) {
    std::optional<std::string> content_type;
    std::optional<std::string> body =
        ExtractBody(isolate, *init.body, &content_type);
    if (!body) {
      return nullptr;
    }
    request->body_ = std::move(body);
    if (content_type && !request->headers_->GetValue("content-type")) {
      request->headers_->AppendLine("Content-Type", *content_type);
    }
  }
  return request;
}

// static
Request* Request::CreateFromStored(
    GURL url,
    std::string method,
    const std::vector<std::pair<std::string, std::string>>& headers) {
  Request* const request = bindings::MakeGarbageCollected<Request>();
  request->url_ = std::move(url);
  request->method_ = std::move(method);
  request->signal_ = AbortSignal::CreateComposite({});
  request->headers_ = Headers::Create(Headers::Guard::kRequest);
  for (const auto& [name, value] : headers) {
    request->headers_->AppendLine(name, value);
  }
  return request;
}

Request::Request() = default;
Request::~Request() = default;

const bindings::WrapperTypeInfo* Request::GetWrapperTypeInfo() const {
  return &bindings::V8Request::kWrapperTypeInfo;
}

void Request::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(headers_);
  visitor->Trace(signal_);
}

std::u16string Request::Url() const {
  return base::UTF8ToUTF16(url_.spec());
}

}  // namespace mitto
