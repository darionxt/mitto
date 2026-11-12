#include "mitto/dom/response.h"

#include "base/check.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_response.h"
#include "mitto/dom/fetch_loader.h"
#include "mitto/dom/headers.h"
#include "mitto/dom/request.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-json.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

bool IsNullBodyStatus(int32_t status) {
  return status == 101 || status == 103 || status == 204 || status == 205 ||
         status == 304;
}

// HTTP's reason-phrase: tabs, spaces, visible ASCII and obs-text.
bool IsValidReasonPhrase(std::string_view text) {
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (!(byte == 0x09 || byte == 0x20 || (byte >= 0x21 && byte <= 0x7E) ||
          byte >= 0x80)) {
      return false;
    }
  }
  return true;
}

// The Encoding Standard's UTF-8 decode of a whole body: a byte order mark
// goes, and each sequence that is not UTF-8 becomes U+FFFD.
std::u16string DecodeUtf8(std::string_view bytes) {
  if (bytes.starts_with("\xEF\xBB\xBF")) {
    bytes.remove_prefix(3);
  }
  std::u16string text;
  base::UTF8ToUTF16(bytes.data(), bytes.size(), &text);
  return text;
}

// The Response constructor's steps after its body is extracted.
Response* Initialize(std::optional<std::string> body,
                     std::optional<std::string> content_type,
                     const bindings::ResponseInit& init,
                     bindings::ExceptionState& exception_state) {
  const uint16_t status = init.status;
  if (status < 200 || status > 599) {
    exception_state.ThrowRangeError(
        base::StrCat({"The status provided (", base::NumberToString(status),
                      ") is outside the range [200, 599]."}));
    return nullptr;
  }
  if (!IsValidReasonPhrase(init.status_text)) {
    exception_state.ThrowTypeError("Invalid statusText");
    return nullptr;
  }
  Headers* const headers = Headers::Create(Headers::Guard::kResponse);
  if (init.headers) {
    headers->FillWith(*init.headers, exception_state);
    if (exception_state.HadException()) {
      return nullptr;
    }
  }
  if (body && IsNullBodyStatus(status)) {
    exception_state.ThrowTypeError(
        "Response with null body status cannot have body");
    return nullptr;
  }
  if (content_type && !headers->GetValue("content-type")) {
    headers->AppendLine("Content-Type", *content_type);
  }
  Response* const response = Response::CreateFetched(
      Response::ResponseType::kDefault, status, init.status_text,
      std::u16string(),
      /*redirected=*/false, headers, /*loader=*/nullptr);
  if (body) {
    response->AppendBody(*body);
    response->EndBody(0);
  }
  return response;
}

}  // namespace

// static
Response* Response::Create(const std::optional<v8::Local<v8::Value>>& body,
                           const bindings::ResponseInit& init,
                           bindings::ExceptionState& exception_state) {
  std::optional<std::string> bytes;
  std::optional<std::string> content_type;
  if (body && !(*body)->IsNullOrUndefined()) {
    bytes = ExtractBody(v8::Isolate::GetCurrent(), *body, &content_type);
    if (!bytes) {
      return nullptr;
    }
  }
  return Initialize(std::move(bytes), std::move(content_type), init,
                    exception_state);
}

// static
Response* Response::StaticJson(v8::Local<v8::Value> data,
                               const bindings::ResponseInit& init,
                               bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::String> json;
  if (!v8::JSON::Stringify(isolate->GetCurrentContext(), data).ToLocal(&json)) {
    return nullptr;
  }
  const v8::String::Utf8Value text(isolate, json);
  // SAFETY: Utf8Value holds length() bytes at *text.
  std::string bytes(UNSAFE_BUFFERS(std::string_view(*text, text.length())));
  // What JSON.stringify() writes nothing for comes back as "undefined".
  if (bytes == "undefined") {
    exception_state.ThrowTypeError("The data is not JSON serializable");
    return nullptr;
  }
  return Initialize(std::move(bytes), "application/json", init,
                    exception_state);
}

// static
Response* Response::CreateFetched(ResponseType type,
                                  int32_t status,
                                  std::string status_text,
                                  std::u16string url,
                                  bool redirected,
                                  mitto::Headers* headers,
                                  FetchLoader* loader) {
  Response* const response = bindings::MakeGarbageCollected<Response>();
  response->type_ = type;
  response->status_ = status;
  response->status_text_ = std::move(status_text);
  response->url_ = std::move(url);
  response->redirected_ = redirected;
  response->headers_ = headers;
  if (loader) {
    response->body_state_ = BodyState::kLoading;
    response->loader_ = loader;
  }
  return response;
}

Response::Response() = default;
Response::~Response() = default;

const bindings::WrapperTypeInfo* Response::GetWrapperTypeInfo() const {
  return &bindings::V8Response::kWrapperTypeInfo;
}

void Response::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(headers_);
  visitor->Trace(reader_);
}

std::u16string Response::Type() const {
  switch (type_) {
    case ResponseType::kBasic:
      return u"basic";
    case ResponseType::kCors:
      return u"cors";
    case ResponseType::kDefault:
      return u"default";
  }
}

v8::Local<v8::Promise> Response::ArrayBuffer(
    bindings::ExceptionState& exception_state) {
  return Read(ReadAs::kArrayBuffer, exception_state);
}

v8::Local<v8::Promise> Response::Json(
    bindings::ExceptionState& exception_state) {
  return Read(ReadAs::kJson, exception_state);
}

v8::Local<v8::Promise> Response::Text(
    bindings::ExceptionState& exception_state) {
  return Read(ReadAs::kText, exception_state);
}

void Response::AppendBody(std::string_view bytes) {
  body_.append(bytes);
  if (body_state_ == BodyState::kNull) {
    body_state_ = BodyState::kLoading;
  }
}

void Response::EndBody(int net_error) {
  // A null body stays one, whenever the end comes.
  if (body_state_ == BodyState::kNull) {
    return;
  }
  body_state_ = net_error == 0 ? BodyState::kComplete : BodyState::kFailed;
  if (!whole_body_reader_.is_null()) {
    std::move(whole_body_reader_).Run(TakeBody());
    return;
  }
  if (reader_.IsEmpty()) {
    return;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::HandleScope handle_scope(isolate);
  const v8::Local<v8::Promise::Resolver> resolver = reader_.Get(isolate);
  reader_.Reset();
  Settle(resolver, reader_read_as_);
}

v8::Local<v8::Promise> Response::Read(
    ReadAs read_as,
    bindings::ExceptionState& exception_state) {
  // Upstream: third_party/blink/renderer/core/fetch/body.cc
  if (body_used_) {
    exception_state.ThrowTypeError("body stream already read");
    return v8::Local<v8::Promise>();
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(isolate->GetCurrentContext())
           .ToLocal(&resolver)) {
    return v8::Local<v8::Promise>();
  }
  // A null body is never used, and reads as empty each time.
  if (body_state_ != BodyState::kNull) {
    body_used_ = true;
  }
  if (body_state_ == BodyState::kLoading) {
    reader_.Reset(isolate, resolver);
    reader_read_as_ = read_as;
    if (loader_) {
      loader_->ReaderStarted();
    }
  } else {
    Settle(resolver, read_as);
  }
  return resolver->GetPromise();
}

void Response::ReadAll(
    base::OnceCallback<void(std::optional<std::string> body)> done) {
  CHECK(!body_used_);
  if (body_state_ == BodyState::kNull) {
    std::move(done).Run(std::string());
    return;
  }
  body_used_ = true;
  if (body_state_ != BodyState::kLoading) {
    std::move(done).Run(TakeBody());
    return;
  }
  whole_body_reader_ = std::move(done);
  if (loader_) {
    loader_->ReaderStarted();
  }
}

std::optional<std::string> Response::TakeBody() {
  if (body_state_ == BodyState::kFailed) {
    return std::nullopt;
  }
  std::string body;
  body.swap(body_);
  return body;
}

void Response::Settle(v8::Local<v8::Promise::Resolver> resolver,
                      ReadAs read_as) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  if (body_state_ == BodyState::kFailed) {
    // Upstream: third_party/blink/renderer/core/fetch/body.cc
    std::ignore = resolver->Reject(
        context, v8::Exception::TypeError(v8::String::NewFromUtf8Literal(
                     isolate, "Failed to fetch")));
    return;
  }
  std::string body;
  body.swap(body_);
  switch (read_as) {
    case ReadAs::kArrayBuffer: {
      const v8::Local<v8::ArrayBuffer> buffer =
          v8::ArrayBuffer::New(isolate, body.size());
      if (!body.empty()) {
        // SAFETY: the new buffer holds body.size() bytes.
        UNSAFE_BUFFERS(
            base::span(static_cast<uint8_t*>(buffer->Data()), body.size()))
            .copy_from(base::as_byte_span(body));
      }
      std::ignore = resolver->Resolve(context, buffer);
      return;
    }
    case ReadAs::kText:
      std::ignore = resolver->Resolve(
          context, bindings::ToV8String(isolate, DecodeUtf8(body)));
      return;
    case ReadAs::kJson: {
      const v8::TryCatch try_catch(isolate);
      v8::Local<v8::Value> value;
      if (!v8::JSON::Parse(context,
                           bindings::ToV8String(isolate, DecodeUtf8(body)))
               .ToLocal(&value)) {
        if (try_catch.HasCaught()) {
          std::ignore = resolver->Reject(context, try_catch.Exception());
        }
        return;
      }
      std::ignore = resolver->Resolve(context, value);
      return;
    }
  }
}

void Response::Dispose() {
  if (loader_) {
    loader_->ResponseCollected();
    loader_ = nullptr;
  }
}

}  // namespace mitto
