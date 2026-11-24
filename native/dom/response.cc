#include "mitto/dom/response.h"

#include <utility>

#include "base/check.h"
#include "base/compiler_specific.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/v8_readable_stream.h"
#include "mitto/bindings/v8_response.h"
#include "mitto/dom/body_stream_buffer.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/fetch_data_loader.h"
#include "mitto/dom/form_data_bytes_consumer.h"
#include "mitto/dom/headers.h"
#include "mitto/dom/readable_stream.h"
#include "mitto/dom/request.h"
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

// The Response constructor's steps after its body is extracted, with `body`
// null for a null body.
Response* Initialize(BodyStreamBuffer* body,
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
  if (body) {
    if (IsNullBodyStatus(status)) {
      exception_state.ThrowTypeError(
          "Response with null body status cannot have body");
      return nullptr;
    }
    // Extracting a ReadableStream: if it is disturbed or locked, throw a
    // TypeError. A body of anything else is neither.
    if (body->IsStreamLocked() || body->IsStreamDisturbed()) {
      exception_state.ThrowTypeError(
          "Response body object should not be disturbed or locked");
      return nullptr;
    }
    if (content_type && !headers->GetValue("content-type")) {
      headers->AppendLine("Content-Type", *content_type);
    }
  }
  return Response::CreateFetched(Response::ResponseType::kDefault, status,
                                 init.status_text, std::u16string(),
                                 /*redirected=*/false, headers, body);
}

// What ReadAll() reads the body with. An abort and then the loader's failure
// may both come, as a promise of Blink's settles once whatever settles it
// after.
class ReadAllClient final : public cppgc::GarbageCollected<ReadAllClient>,
                            public FetchDataLoader::Client {
 public:
  explicit ReadAllClient(
      base::OnceCallback<void(std::optional<std::string>, bool)> done)
      : done_(std::move(done)) {}

  void DidFetchDataLoadedArrayBuffer(std::string bytes) override {
    Done(std::move(bytes), /*aborted=*/false);
  }
  void DidFetchDataLoadFailed() override {
    Done(std::nullopt, /*aborted=*/false);
  }
  void Abort(v8::Local<v8::Value> reason) override {
    Done(std::nullopt, /*aborted=*/true);
  }

 private:
  void Done(std::optional<std::string> body, bool aborted) {
    if (done_) {
      std::move(done_).Run(std::move(body), aborted);
    }
  }

  base::OnceCallback<void(std::optional<std::string>, bool)> done_;
};

}  // namespace

// static
Response* Response::Create(const std::optional<v8::Local<v8::Value>>& body,
                           const bindings::ResponseInit& init,
                           bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  BodyStreamBuffer* body_buffer = nullptr;
  std::optional<std::string> content_type;
  if (body && !(*body)->IsNullOrUndefined()) {
    if (ReadableStream* const stream =
            bindings::V8ReadableStream::ToWrappable(isolate, *body)) {
      body_buffer =
          bindings::MakeGarbageCollected<BodyStreamBuffer>(isolate, stream);
    } else {
      body_buffer = ExtractBody(isolate, *body, &content_type, exception_state);
      if (!body_buffer) {
        return nullptr;
      }
    }
  }
  return Initialize(body_buffer, std::move(content_type), init,
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
  BodyStreamBuffer* const body_buffer = BodyStreamBuffer::Create(
      isolate,
      bindings::MakeGarbageCollected<FormDataBytesConsumer>(std::move(bytes)),
      /*signal=*/nullptr);
  return Initialize(body_buffer, "application/json", init, exception_state);
}

// static
Response* Response::CreateFetched(ResponseType type,
                                  int32_t status,
                                  std::string status_text,
                                  std::u16string url,
                                  bool redirected,
                                  mitto::Headers* headers,
                                  BodyStreamBuffer* body) {
  Response* const response = bindings::MakeGarbageCollected<Response>();
  response->type_ = type;
  response->status_ = status;
  response->status_text_ = std::move(status_text);
  response->url_ = std::move(url);
  response->redirected_ = redirected;
  response->headers_ = headers;
  if (type == ResponseType::kOpaque) {
    response->internal_body_buffer_ = body;
  } else {
    response->body_buffer_ = body;
  }
  return response;
}

// static
Response* Response::Error() {
  // Blink's CreateNetworkErrorResponse().
  Response* const response = bindings::MakeGarbageCollected<Response>();
  response->type_ = ResponseType::kError;
  response->status_ = 0;
  response->headers_ = mitto::Headers::Create(Headers::Guard::kImmutable);
  return response;
}

// static
Response* Response::Redirect(const std::u16string& url,
                             uint16_t status,
                             bindings::ExceptionState& exception_state) {
  // As ExecutionContext::CompleteURL() parses it, with a document's encoding
  // for its query.
  const GURL parsed = ExecutionContext::Current().CompleteUrl(url);
  if (!parsed.is_valid()) {
    exception_state.ThrowTypeError(
        base::StrCat({"Failed to parse URL from ", base::UTF16ToUTF8(url)}));
    return nullptr;
  }
  if (status != 301 && status != 302 && status != 303 && status != 307 &&
      status != 308) {
    exception_state.ThrowRangeError("Invalid status code");
    return nullptr;
  }
  Response* const response = bindings::MakeGarbageCollected<Response>();
  response->status_ = status;
  response->headers_ = mitto::Headers::Create(Headers::Guard::kImmutable);
  response->headers_->AppendLine("Location", parsed.spec());
  return response;
}

Response::Response() = default;
Response::~Response() = default;

Response* Response::Clone(bindings::ExceptionState& exception_state) {
  if (IsBodyLocked() || IsBodyUsed()) {
    exception_state.ThrowTypeError("Response body is already used");
    return nullptr;
  }
  // FetchResponseData::Clone(): the body goes to both through a tee, and so
  // does an opaque response's internal body, which each of them keeps loading.
  Response* const response = bindings::MakeGarbageCollected<Response>();
  response->type_ = type_;
  response->status_ = status_;
  response->status_text_ = status_text_;
  response->url_ = url_;
  response->redirected_ = redirected_;
  response->headers_ = headers_->Clone();
  for (const auto member :
       {&Response::body_buffer_, &Response::internal_body_buffer_}) {
    BodyStreamBuffer* const body = (this->*member).Get();
    if (!body) {
      continue;
    }
    BodyStreamBuffer* branch1 = nullptr;
    BodyStreamBuffer* branch2 = nullptr;
    body->Tee(&branch1, &branch2, exception_state);
    if (exception_state.HadException()) {
      return nullptr;
    }
    this->*member = branch1;
    response->*member = branch2;
  }
  return response;
}

std::string Response::ContentType() const {
  return headers_->GetValue("content-type").value_or("");
}

BodyStreamBuffer* Response::BodyBuffer() const {
  return body_buffer_.Get();
}

const bindings::WrapperTypeInfo* Response::GetWrapperTypeInfo() const {
  return &bindings::V8Response::kWrapperTypeInfo;
}

void Response::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(headers_);
  visitor->Trace(body_buffer_);
  visitor->Trace(internal_body_buffer_);
}

std::u16string Response::Type() const {
  switch (type_) {
    case ResponseType::kBasic:
      return u"basic";
    case ResponseType::kCors:
      return u"cors";
    case ResponseType::kDefault:
      return u"default";
    case ResponseType::kOpaque:
      return u"opaque";
    case ResponseType::kError:
      return u"error";
  }
}

void Response::ReadAll(base::OnceCallback<void(std::optional<std::string> body,
                                               bool aborted)> done) {
  CHECK(!IsBodyLocked() && !IsBodyUsed());
  if (!body_buffer_) {
    std::move(done).Run(std::string(), /*aborted=*/false);
    return;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  auto* const client =
      bindings::MakeGarbageCollected<ReadAllClient>(std::move(done));
  bindings::ExceptionState exception_state(isolate);
  const v8::TryCatch try_catch(isolate);
  body_buffer_->StartLoading(FetchDataLoader::CreateLoaderAsArrayBuffer(),
                             client, exception_state);
  if (exception_state.HadException()) {
    client->DidFetchDataLoadFailed();
  }
}

}  // namespace mitto
