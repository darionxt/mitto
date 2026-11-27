#include "mitto/dom/xml_http_request.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>
#include <string_view>

#include "base/check.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/notreached.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/per_context_data.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_xml_document.h"
#include "mitto/bindings/v8_xml_http_request.h"
#include "mitto/dom/blob.h"
#include "mitto/dom/document.h"
#include "mitto/dom/event.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/file_reader_loader.h"
#include "mitto/dom/form_data.h"
#include "mitto/dom/html_document.h"
#include "mitto/dom/local_scheme_fetch.h"
#include "mitto/dom/markup_serializer.h"
#include "mitto/dom/page_dismissal_scope.h"
#include "mitto/dom/parsed_content_type.h"
#include "mitto/dom/stream_chunks.h"
#include "mitto/dom/url_search_params.h"
#include "mitto/dom/window.h"
#include "mitto/dom/window_task.h"
#include "mitto/dom/xml_document.h"
#include "mitto/dom/xml_http_request_loader.h"
#include "mitto/dom/xml_http_request_progress_event_throttle.h"
#include "mitto/dom/xml_http_request_upload.h"
#include "mitto/encoding/content_type.h"
#include "mitto/encoding/text_encoding_registry.h"
#include "mitto/encoding/text_resource_decoder_builder.h"
#include "mitto/header_rules.h"
#include "third_party/abseil-cpp/absl/functional/overload.h"
#include "url/origin.h"
#include "url/url_constants.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-json.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

// net's errors, which the Page program, with no //net, cannot name.
constexpr int kFailed = -2;
constexpr int kAborted = -3;
constexpr int kTimedOut = -7;

// HttpResponseHeaders::GetContentLength() of the Content-Length `field`: what
// its first value says, or -1.
int64_t ContentLength(std::string_view field) {
  const std::string_view first = base::TrimWhitespaceASCII(
      field.substr(0, field.find(',')), base::TRIM_ALL);
  int64_t length = -1;
  if (first.starts_with('+') || !base::StringToInt64(first, &length)) {
    return -1;
  }
  return std::max<int64_t>(length, -1);
}

// Blink's FindCharsetInMediaType(): where the charset parameter's value is in
// `media_type`, from `position`, and how long it is, 0 for none.
std::pair<size_t, size_t> FindCharsetInMediaType(std::string_view media_type,
                                                 size_t position) {
  const std::string lower = base::ToLowerASCII(media_type);
  const size_t length = media_type.size();
  while (position < length) {
    position = lower.find("charset", position);
    // Give up if "charset" is at the head.
    if (position == std::string::npos || position == 0) {
      return {0, 0};
    }
    // "charset" may be part of a longer word.
    const char before = media_type[position - 1];
    if (static_cast<unsigned char>(before) > ' ' && before != ';') {
      position += 7;
      continue;
    }
    position += 7;
    while (position < length &&
           static_cast<unsigned char>(media_type[position]) <= ' ') {
      ++position;
    }
    // Treat this as a charset parameter.
    if (position < length && media_type[position++] == '=') {
      break;
    }
  }
  while (position < length &&
         (static_cast<unsigned char>(media_type[position]) <= ' ' ||
          media_type[position] == '"' || media_type[position] == '\'')) {
    ++position;
  }
  size_t end = position;
  while (end < length && static_cast<unsigned char>(media_type[end]) > ' ' &&
         media_type[end] != '"' && media_type[end] != '\'' &&
         media_type[end] != ';') {
    ++end;
  }
  return {position, end - position};
}

// FilterSourceStream::GetContentEncodingTypes(): whether //net decodes a body
// of the Content-Encoding `field`, which it leaves as it is if it does not know
// one of its encodings.
bool IsDecodedBody(std::string_view field) {
  const std::vector<std::string_view> encodings = base::SplitStringPiece(
      field, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
  return !encodings.empty() &&
         std::ranges::all_of(encodings, [](std::string_view encoding) {
           return std::ranges::any_of(
               std::to_array<std::string_view>(
                   {"br", "deflate", "gzip", "x-gzip", "zstd"}),
               [&](std::string_view known) {
                 return base::EqualsCaseInsensitiveASCII(encoding, known);
               });
         });
}

std::string ExtractCharsetFromMediaType(std::string_view media_type) {
  const auto [position, length] = FindCharsetInMediaType(media_type, 0);
  return std::string(media_type.substr(position, length));
}

std::string ReplaceCharsetInMediaType(std::string media_type,
                                      std::string_view charset) {
  size_t position = 0;
  while (true) {
    const auto [found, length] = FindCharsetInMediaType(media_type, position);
    if (length == 0) {
      return media_type;
    }
    media_type.replace(found, length, charset);
    position = found + charset.size();
  }
}

// A ByteString's bytes, each a character, as a message shows them.
std::string ToUtf8(std::string_view byte_string) {
  std::u16string text;
  text.reserve(byte_string.size());
  for (const char byte : byte_string) {
    text.push_back(static_cast<unsigned char>(byte));
  }
  return base::UTF16ToUTF8(text);
}

// KURL::ElidedString().
std::string ElidedUrl(const GURL& url) {
  const std::string& spec = url.possibly_invalid_spec();
  if (spec.size() <= 1024) {
    return spec;
  }
  return base::StrCat(
      {spec.substr(0, 511), "...", spec.substr(spec.size() - 510)});
}

bool ValidateOpenArguments(std::string_view method,
                           const GURL& url,
                           bindings::ExceptionState& exception_state) {
  if (!IsHttpToken(method)) {
    exception_state.ThrowDOMException(
        "SyntaxError",
        base::StrCat({"'", ToUtf8(method), "' is not a valid HTTP method."}));
    return false;
  }
  if (IsForbiddenMethod(method)) {
    exception_state.ThrowDOMException(
        "SecurityError",
        base::StrCat({"'", method, "' HTTP method is unsupported."}));
    return false;
  }
  if (!url.is_valid()) {
    exception_state.ThrowDOMException("SyntaxError", "Invalid URL");
    return false;
  }
  return true;
}

// FetchUtils::NormalizeMethod(): the methods that go in upper case.
std::string NormalizeMethod(std::string_view method) {
  for (const std::string_view upper :
       {"GET", "POST", "DELETE", "HEAD", "OPTIONS", "PUT"}) {
    if (base::EqualsCaseInsensitiveASCII(method, upper)) {
      return std::string(upper);
    }
  }
  return std::string(method);
}

}  // namespace

// Reads a response that came as a blob back to its XMLHttpRequest, whose
// responseType has changed since send(), from tasks of file reading, as
// Blink's BlobLoader does.
class XMLHttpRequest::BlobLoader final
    : public cppgc::GarbageCollected<XMLHttpRequest::BlobLoader>,
      public FileReaderClient {
 public:
  BlobLoader(XMLHttpRequest* xhr, scoped_refptr<BlobDataHandle> blob)
      : xhr_(xhr),
        loader_(bindings::MakeGarbageCollected<FileReaderLoader>(
            this,
            xhr->GetExecutionContext())) {
    loader_->Start(std::move(blob));
  }

  void Cancel() { loader_->Cancel(); }

  // FileReaderClient:
  FileErrorCode DidStartLoading(uint64_t total_bytes) override {
    return FileErrorCode::kOK;
  }
  FileErrorCode DidReceiveData(base::span<const uint8_t> data) override {
    xhr_->DidReceiveData(base::as_string_view(data));
    return FileErrorCode::kOK;
  }
  void DidFinishLoading() override { xhr_->DidFinishLoadingFromBlob(); }
  void DidFail(FileErrorCode code) override { xhr_->DidFailLoadingFromBlob(); }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(xhr_);
    visitor->Trace(loader_);
  }

 private:
  const cppgc::Member<XMLHttpRequest> xhr_;
  const cppgc::Member<FileReaderLoader> loader_;
};

// static
XMLHttpRequest* XMLHttpRequest::Create() {
  return bindings::MakeGarbageCollected<XMLHttpRequest>(
      &ExecutionContext::Current());
}

XMLHttpRequest::XMLHttpRequest(ExecutionContext* context)
    : context_(context),
      progress_event_throttle_(
          bindings::MakeGarbageCollected<XMLHttpRequestProgressEventThrottle>(
              this)),
      made_without_context_(!context->HasContext()) {}

XMLHttpRequest::~XMLHttpRequest() {
  memory_accounter_.Decrease(v8::Isolate::GetCurrent(),
                             static_cast<size_t>(reported_memory_));
}

const bindings::WrapperTypeInfo* XMLHttpRequest::GetWrapperTypeInfo() const {
  return &bindings::V8XMLHttpRequest::kWrapperTypeInfo;
}

void XMLHttpRequest::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(context_);
  visitor->Trace(progress_event_throttle_);
  visitor->Trace(upload_);
  visitor->Trace(loader_);
  visitor->Trace(blob_loader_);
  visitor->Trace(response_document_);
  visitor->Trace(response_blob_);
  visitor->Trace(response_array_buffer_);
  XMLHttpRequestEventTarget::Trace(visitor);
}

ExecutionContext* XMLHttpRequest::GetExecutionContext() const {
  ExecutionContext* const context = context_.Get();
  return context && (made_without_context_ || context->HasContext()) ? context
                                                                     : nullptr;
}

bool XMLHttpRequest::CanDispatch() const {
  ExecutionContext* const context = context_.Get();
  return context && context->HasContext();
}

void XMLHttpRequest::DispatchReadyStateChange() {
  bindings::EventInit init;
  Event* const event = Event::Create(u"readystatechange", init);
  event->SetTrusted(true);
  DispatchEvent(event);
}

// The response's members.

std::u16string XMLHttpRequest::ResponseText(
    bindings::ExceptionState& exception_state) {
  CheckContext();
  if (response_type_ != ResponseTypeCode::kDefault &&
      response_type_ != ResponseTypeCode::kText) {
    exception_state.ThrowDOMException(
        "InvalidStateError",
        base::StrCat({"The value is only accessible if the object's "
                      "'responseType' is '' or 'text' (was '",
                      base::UTF16ToUTF8(ResponseType()), "')."}));
    return std::u16string();
  }
  if (error_ || (state_ != kLoading && state_ != kDone)) {
    return std::u16string();
  }
  return response_text_;
}

Document* XMLHttpRequest::ResponseXML(
    bindings::ExceptionState& exception_state) {
  CheckContext();
  if (response_type_ != ResponseTypeCode::kDefault &&
      response_type_ != ResponseTypeCode::kDocument) {
    exception_state.ThrowDOMException(
        "InvalidStateError",
        base::StrCat({"The value is only accessible if the object's "
                      "'responseType' is '' or 'document' (was '",
                      base::UTF16ToUTF8(ResponseType()), "')."}));
    return nullptr;
  }
  if (error_ || state_ != kDone) {
    return nullptr;
  }
  if (!parsed_response_) {
    ParseResponseDocument();
  }
  return response_document_.Get();
}

void XMLHttpRequest::InitResponseDocument() {
  response_document_ = nullptr;
  // The final MIME type has to be an XML one, or text/html with a responseType
  // of "document".
  const bool is_html = ResponseIsHtml();
  const bool is_http = response_ && GURL(response_->url).SchemeIsHTTPOrHTTPS();
  ExecutionContext* const context = GetExecutionContext();
  if ((is_http && !ResponseIsXml() && !is_html) ||
      (is_html && response_type_ == ResponseTypeCode::kDefault) || !context ||
      !context->IsWindow()) {
    return;
  }
  const std::u16string content_type = base::UTF8ToUTF16(ResponseMimeType());
  if (is_html) {
    response_document_ = HTMLDocument::Create();
    response_document_->SetAllowDeclarativeShadowRoots(false);
  } else {
    response_document_ = XMLDocument::Create(content_type);
  }
  const Document& creator = static_cast<Window*>(context)->GetDocument();
  // As in Blink, a listener may have let the response go.
  if (response_) {
    response_document_->SetUrl(base::UTF8ToUTF16(response_->url));
  }
  response_document_->SetOrigin(creator.GetOrigin());
  response_document_->SetContentType(content_type);
}

void XMLHttpRequest::ParseResponseDocument() {
  // An HTML one is there from its first bytes on.
  if (!response_document_) {
    InitResponseDocument();
  }
  if (!response_document_) {
    return;
  }
  // Its encoding is the decoder's, which an HTML one's parser has.
  if (decoder_ && response_document_->IsHTMLDocument()) {
    response_document_->SetEncoding(
        decoder_->encoding(), decoder_->EncodingWasDetectedHeuristically(),
        decoder_->saw_error());
  }
  response_document_->SetContent(response_text_);
  // An HTML one is parsed as it comes, with no load to complete and no text
  // kept, which no script can read.
  if (response_document_->IsHTMLDocument()) {
    response_document_->SetReadiness(Document::Readiness::kInteractive);
    response_text_ = std::u16string();
    ReportMemoryUsageToV8();
  }
  if (!response_document_->WellFormed()) {
    response_document_ = nullptr;
  } else if (const std::optional<std::string> modified =
                 ResponseHeader("last-modified")) {
    base::Time time;
    if (base::Time::FromUTCString(modified->c_str(), &time)) {
      response_document_->SetLastModified(time);
    }
  }
  parsed_response_ = true;
}

v8::Local<v8::Value> XMLHttpRequest::ResponseJson() const {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Context> context =
      bindings::ReceiverRealmScope::CreationContext(isolate);
  const v8::Context::Scope context_scope(context);
  // An error of the syntax gives null, as the standard says.
  const v8::TryCatch try_catch(isolate);
  v8::Local<v8::Value> json;
  if (!v8::JSON::Parse(context, bindings::ToV8String(isolate, response_text_))
           .ToLocal(&json)) {
    return v8::Null(isolate);
  }
  return json;
}

Blob* XMLHttpRequest::ResponseBlob() {
  if (!response_blob_) {
    std::string type = base::ToLowerASCII(ResponseMimeType());
    if (!IsValidBlobType(type)) {
      type.clear();
    }
    // The bytes go to the blob, with no copy.
    response_blob_ = mitto::Blob::Create(BlobDataHandle::Create(
        std::exchange(binary_response_, std::string()), std::move(type)));
    ReportMemoryUsageToV8();
  }
  return response_blob_.Get();
}

v8::Local<v8::Value> XMLHttpRequest::ResponseArrayBuffer() {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (response_array_buffer_.IsEmpty() && !response_array_buffer_failure_) {
    const v8::Context::Scope context_scope(
        bindings::ReceiverRealmScope::CreationContext(isolate));
    // A buffer that cannot be had is null, now and for each later call, and
    // the bytes go.
    v8::Local<v8::ArrayBuffer> buffer;
    if (MaybeNewArrayBuffer(isolate, base::as_byte_span(binary_response_))
            .ToLocal(&buffer)) {
      response_array_buffer_.Reset(isolate, buffer);
    } else {
      response_array_buffer_failure_ = true;
    }
    binary_response_ = std::string();
    ReportMemoryUsageToV8();
  }
  if (response_array_buffer_.IsEmpty()) {
    return v8::Null(isolate);
  }
  return response_array_buffer_.Get(isolate);
}

v8::Local<v8::Value> XMLHttpRequest::Response() {
  CheckContext();
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  // Text can be read while it loads.
  if (response_type_ == ResponseTypeCode::kDefault ||
      response_type_ == ResponseTypeCode::kText) {
    bindings::ExceptionState exception_state(isolate);
    return bindings::ToV8String(isolate, ResponseText(exception_state));
  }
  if (error_ || state_ != kDone) {
    return v8::Null(isolate);
  }
  switch (response_type_) {
    case ResponseTypeCode::kJson:
      return ResponseJson();
    case ResponseTypeCode::kDocument: {
      bindings::ExceptionState exception_state(isolate);
      return bindings::ToV8(isolate, ResponseXML(exception_state));
    }
    case ResponseTypeCode::kBlob:
      return bindings::ToV8(isolate, ResponseBlob());
    case ResponseTypeCode::kArrayBuffer:
      return ResponseArrayBuffer();
    case ResponseTypeCode::kDefault:
    case ResponseTypeCode::kText:
      break;
  }
  NOTREACHED();
}

std::u16string XMLHttpRequest::ResponseType() const {
  switch (response_type_) {
    case ResponseTypeCode::kDefault:
      return u"";
    case ResponseTypeCode::kArrayBuffer:
      return u"arraybuffer";
    case ResponseTypeCode::kBlob:
      return u"blob";
    case ResponseTypeCode::kDocument:
      return u"document";
    case ResponseTypeCode::kJson:
      return u"json";
    case ResponseTypeCode::kText:
      return u"text";
  }
}

void XMLHttpRequest::SetResponseType(
    const std::u16string& response_type,
    bindings::ExceptionState& exception_state) {
  ExecutionContext* const context = GetExecutionContext();
  const bool is_window = context && context->IsWindow();
  if (!is_window && response_type == u"document") {
    return;
  }
  if (state_ >= kLoading) {
    exception_state.ThrowDOMException(
        "InvalidStateError",
        "The response type cannot be set if the object's state is LOADING or "
        "DONE.");
    return;
  }
  if (is_window && !async_) {
    exception_state.ThrowDOMException(
        "InvalidAccessError",
        "The response type cannot be changed for synchronous requests made "
        "from a document.");
    return;
  }
  response_type_ = response_type == u"arraybuffer"
                       ? ResponseTypeCode::kArrayBuffer
                   : response_type == u"blob"     ? ResponseTypeCode::kBlob
                   : response_type == u"document" ? ResponseTypeCode::kDocument
                   : response_type == u"json"     ? ResponseTypeCode::kJson
                   : response_type == u"text"     ? ResponseTypeCode::kText
                                                  : ResponseTypeCode::kDefault;
}

std::u16string XMLHttpRequest::ResponseURL() {
  CheckContext();
  if (!response_) {
    return std::u16string();
  }
  return base::UTF8ToUTF16(GURL(response_->url).GetWithoutRef().spec());
}

uint16_t XMLHttpRequest::Status() {
  CheckContext();
  if (state_ == kUnsent || state_ == kOpened || error_ || !response_) {
    return 0;
  }
  return static_cast<uint16_t>(response_->status);
}

std::string XMLHttpRequest::StatusText() {
  CheckContext();
  if (state_ == kUnsent || state_ == kOpened || error_ || !response_) {
    return std::string();
  }
  return response_->status_text;
}

std::optional<std::string> XMLHttpRequest::ResponseHeader(
    std::string_view name) const {
  for (const auto& [line_name, value] : response_headers_) {
    if (base::EqualsCaseInsensitiveASCII(line_name, name)) {
      return value;
    }
  }
  return std::nullopt;
}

bool XMLHttpRequest::IsExposed(std::string_view name) const {
  if (!response_ || !response_->cors ||
      IsCorsSafelistedResponseHeaderName(name)) {
    return true;
  }
  return exposed_names_.contains(base::ToLowerASCII(name)) ||
         // With credentials, "*" is a name like any other.
         (!with_credentials_ && exposed_names_.contains("*"));
}

std::optional<std::string> XMLHttpRequest::GetResponseHeader(
    const std::string& name) {
  CheckContext();
  if (state_ < kHeadersReceived || error_) {
    return std::nullopt;
  }
  if (IsForbiddenResponseHeaderName(name) || !IsExposed(name)) {
    if (ExecutionContext* const context = GetExecutionContext()) {
      context->AddConsoleMessage(
          ConsoleLevel::kError, base::StrCat({"Refused to get unsafe header \"",
                                              ToUtf8(name), "\""}));
    }
    return std::nullopt;
  }
  return ResponseHeader(name);
}

std::string XMLHttpRequest::GetAllResponseHeaders() {
  CheckContext();
  if (state_ < kHeadersReceived || error_) {
    return std::string();
  }
  std::vector<std::pair<std::string, std::string>> headers;
  for (const auto& [name, value] : response_headers_) {
    // The names that a script may not read, which go for every response.
    if (!IsForbiddenResponseHeaderName(name) && IsExposed(name)) {
      headers.emplace_back(base::ToUpperASCII(name), value);
    }
  }
  // Sorted by the names in upper case, as legacy code expects.
  std::ranges::sort(headers, {}, &std::pair<std::string, std::string>::first);
  std::string lines;
  for (const auto& [name, value] : headers) {
    base::StrAppend(&lines, {base::ToLowerASCII(name), ": ", value, "\r\n"});
  }
  return lines;
}

// The request's members.

void XMLHttpRequest::SetTimeout(uint32_t timeout,
                                bindings::ExceptionState& exception_state) {
  ExecutionContext* const context = GetExecutionContext();
  if (context && context->IsWindow() && !async_) {
    exception_state.ThrowDOMException(
        "InvalidAccessError",
        "Timeouts cannot be set for synchronous requests made from a "
        "document.");
    return;
  }
  timeout_ = base::Milliseconds(timeout);
  // It may change while the request runs, and counts from the start.
  if (loader_) {
    loader_->SetTimeout(timeout_);
  }
}

void XMLHttpRequest::SetWithCredentials(
    bool value,
    bindings::ExceptionState& exception_state) {
  if (state_ > kOpened || send_flag_) {
    exception_state.ThrowDOMException(
        "InvalidStateError",
        "The value may only be set if the object's state is UNSENT or "
        "OPENED.");
    return;
  }
  with_credentials_ = value;
}

XMLHttpRequestUpload* XMLHttpRequest::Upload() {
  if (!upload_) {
    upload_ = bindings::MakeGarbageCollected<XMLHttpRequestUpload>(this);
  }
  return upload_.Get();
}

void XMLHttpRequest::OverrideMimeType(
    const std::u16string& mime_type,
    bindings::ExceptionState& exception_state) {
  if (state_ == kLoading || state_ == kDone) {
    exception_state.ThrowDOMException(
        "InvalidStateError",
        "MimeType cannot be overridden when the state is LOADING or DONE.");
    return;
  }
  mime_type_override_ = "application/octet-stream";
  if (!ParsedContentType(mime_type).IsValid()) {
    return;
  }
  const std::string type = base::UTF16ToUTF8(mime_type);
  if (!ExtractMimeTypeFromMediaType(type, /*accept_comma_separated=*/false)) {
    return;
  }
  mime_type_override_ = type;
}

void XMLHttpRequest::SetRequestHeader(
    const std::string& name,
    const std::string& value,
    bindings::ExceptionState& exception_state) {
  if (state_ != kOpened || send_flag_) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      "The object's state must be OPENED.");
    return;
  }
  const std::string_view normalized = NormalizeHeaderValue(value);
  if (!IsHttpToken(name)) {
    exception_state.ThrowDOMException(
        "SyntaxError",
        base::StrCat(
            {"'", ToUtf8(name), "' is not a valid HTTP header field name."}));
    return;
  }
  if (!IsValidHeaderValue(normalized)) {
    exception_state.ThrowDOMException(
        "SyntaxError", base::StrCat({"'", ToUtf8(normalized),
                                     "' is not a valid HTTP header field "
                                     "value."}));
    return;
  }
  // No script sets a line that the browser sets.
  if (IsForbiddenRequestHeader(name, value)) {
    if (ExecutionContext* const context = GetExecutionContext()) {
      context->AddConsoleMessage(
          ConsoleLevel::kError,
          base::StrCat({"Refused to set unsafe header \"", name, "\""}));
    }
    return;
  }
  SetRequestHeaderInternal(name, normalized);
}

std::string* XMLHttpRequest::FindRequestHeader(std::string_view name) {
  for (auto& [line_name, value] : request_headers_) {
    if (base::EqualsCaseInsensitiveASCII(line_name, name)) {
      return &value;
    }
  }
  return nullptr;
}

bool XMLHttpRequest::HasContentTypeRequestHeader() const {
  return std::ranges::any_of(request_headers_, [](const auto& line) {
    return base::EqualsCaseInsensitiveASCII(line.first, "content-type");
  });
}

void XMLHttpRequest::SetRequestHeaderInternal(std::string_view name,
                                              std::string_view value) {
  if (std::string* const current = FindRequestHeader(name)) {
    base::StrAppend(current, {", ", value});
    return;
  }
  request_headers_.emplace_back(name, value);
}

void XMLHttpRequest::UpdateContentTypeAndCharset(
    std::string_view default_content_type,
    std::string_view charset) {
  std::string* const content_type = FindRequestHeader("content-type");
  if (!content_type) {
    SetRequestHeaderInternal("Content-Type", default_content_type);
    return;
  }
  // A charset that the script gave becomes the body's.
  *content_type = ReplaceCharsetInMediaType(*content_type, charset);
}

void XMLHttpRequest::Open(const std::string& method,
                          const std::u16string& url,
                          bindings::ExceptionState& exception_state) {
  Open(method, url, /*async=*/true, std::nullopt, std::nullopt,
       exception_state);
}

void XMLHttpRequest::Open(const std::string& method,
                          const std::u16string& url_string,
                          bool async,
                          const std::optional<std::u16string>& username,
                          const std::optional<std::u16string>& password,
                          bindings::ExceptionState& exception_state) {
  ExecutionContext* const context = GetExecutionContext();
  if (!context) {
    return;
  }
  GURL url = context->CompleteUrl(url_string);
  if (!ValidateOpenArguments(method, url, exception_state)) {
    return;
  }
  if (username || password) {
    GURL::Replacements replacements;
    const std::string user = base::UTF16ToUTF8(username.value_or(u""));
    const std::string pass = base::UTF16ToUTF8(password.value_or(u""));
    if (username) {
      replacements.SetUsernameStr(user);
    }
    if (password) {
      replacements.SetPasswordStr(pass);
    }
    // KURL::SetUser() and SetPass() leave a URL that can have neither as it
    // is.
    if (url.has_host()) {
      url = url.ReplaceComponents(replacements);
    }
  }
  OpenUrl(method, url, async, exception_state);
}

void XMLHttpRequest::OpenUrl(const std::string& method,
                             const GURL& url,
                             bool async,
                             bindings::ExceptionState& exception_state) {
  InternalAbort();

  const State previous_state = state_;
  state_ = kUnsent;
  error_ = false;
  upload_complete_ = false;

  ExecutionContext* const context = GetExecutionContext();
  if (!async && context && context->IsWindow()) {
    if (response_type_ != ResponseTypeCode::kDefault) {
      exception_state.ThrowDOMException(
          "InvalidAccessError",
          "Synchronous requests from a document must not set a response "
          "type.");
      return;
    }
    if (!timeout_.is_zero()) {
      exception_state.ThrowDOMException(
          "InvalidAccessError", "Synchronous requests must not set a timeout.");
      return;
    }
  }

  method_ = NormalizeMethod(method);
  url_ = url;
  blob_url_entry_.reset();
  if (url_.SchemeIsBlob()) {
    blob_url_entry_ = BlobUrlRegistry::Get().ResolveEntry(url_);
  }
  async_ = async;

  DCHECK(!loader_);
  send_flag_ = false;

  // No event for several calls in a row.
  if (previous_state != kOpened) {
    ChangeState(kOpened);
  } else {
    state_ = kOpened;
  }
}

bool XMLHttpRequest::InitSend(bindings::ExceptionState& exception_state) {
  if (!GetExecutionContext() || made_without_context_) {
    HandleNetworkError();
    ThrowForLoadFailureIfNeeded(exception_state,
                                "Document is already detached.");
    return false;
  }
  if (state_ != kOpened || send_flag_) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      "The object's state must be OPENED.");
    return false;
  }
  // A window's synchronous request goes only where its document's permissions
  // policy allows sync-xhr. Its document policy, which Blink also asks, has
  // sync-xhr only behind DocumentPolicySyncXHR, which Chrome 157 has off.
  ExecutionContext* const context = GetExecutionContext();
  if (!async_ && context->IsWindow() &&
      !static_cast<Window*>(context)->GetDocument().IsFeatureEnabled(
          "sync-xhr")) {
    // Upstream: third_party/blink/renderer/core/frame/permissions_policy_violation_report_body.h
    context->AddConsoleMessage(ConsoleLevel::kError,
                               "Permissions policy violation: Synchronous "
                               "requests are disabled by permissions policy.");
    HandleNetworkError();
    ThrowForLoadFailureIfNeeded(exception_state, "");
    return false;
  }
  error_ = false;
  return true;
}

bool XMLHttpRequest::AreMethodAndURLValidForSend() const {
  return method_ != "GET" && method_ != "HEAD" &&
         // SchemeRegistry::ShouldTreatURLSchemeAsSupportingFetchAPI().
         url_.SchemeIsHTTPOrHTTPS();
}

void XMLHttpRequest::Send(
    const std::optional<
        bindings::
            DocumentOrBlobOrArrayBufferOrArrayBufferViewOrFormDataOrURLSearchParamsOrUSVString>&
        body,
    bindings::ExceptionState& exception_state) {
  if (!InitSend(exception_state)) {
    return;
  }
  std::optional<std::string> http_body;
  if (body && AreMethodAndURLValidForSend()) {
    http_body = std::visit(
        absl::Overload{
            [this](Document* document) {
              // A Document that is neither, as new Document() makes, has none.
              if (document->IsHTMLDocument()) {
                UpdateContentTypeAndCharset("text/html;charset=UTF-8", "UTF-8");
              } else if (document->GetWrapperTypeInfo() ==
                         &bindings::V8XMLDocument::kWrapperTypeInfo) {
                UpdateContentTypeAndCharset("application/xml;charset=UTF-8",
                                            "UTF-8");
              }
              return base::UTF16ToUTF8(SerializeNode(*document));
            },
            [this](mitto::Blob* blob) {
              const scoped_refptr<BlobDataHandle> handle =
                  blob->GetBlobDataHandle();
              if (!HasContentTypeRequestHeader()) {
                const std::string_view type =
                    NormalizeHeaderValue(handle->GetType());
                if (!type.empty() &&
                    ParsedContentType(base::UTF8ToUTF16(type)).IsValid()) {
                  SetRequestHeaderInternal("Content-Type", type);
                }
              }
              return std::string(base::as_string_view(handle->Bytes()));
            },
            [](v8::Local<v8::ArrayBuffer> buffer) {
              const std::shared_ptr<v8::BackingStore> store =
                  buffer->GetBackingStore();
              return std::string(static_cast<const char*>(store->Data()),
                                 store->ByteLength());
            },
            [](v8::Local<v8::ArrayBufferView> view) {
              std::string bytes(view->ByteLength(), '\0');
              view->CopyContents(bytes.data(), bytes.size());
              return bytes;
            },
            [this](FormData* form_data) {
              FormData::MultipartBody multipart =
                  form_data->EncodeMultiPartFormData();
              if (!HasContentTypeRequestHeader()) {
                SetRequestHeaderInternal("Content-Type",
                                         multipart.ContentType());
              }
              return std::move(multipart.bytes);
            },
            [this](URLSearchParams* params) {
              UpdateContentTypeAndCharset(
                  "application/x-www-form-urlencoded;charset=UTF-8", "UTF-8");
              return base::UTF16ToUTF8(params->ToString());
            },
            [this](const std::u16string& text) {
              UpdateContentTypeAndCharset("text/plain;charset=UTF-8", "UTF-8");
              return base::UTF16ToUTF8(text);
            },
        },
        *body);
  }
  CreateRequest(std::move(http_body), exception_state);
}

void XMLHttpRequest::ThrowForLoadFailureIfNeeded(
    bindings::ExceptionState& exception_state,
    std::string_view reason) {
  if (error_ && error_code_ == ErrorCode::kNone) {
    error_code_ = ErrorCode::kNetworkError;
  }
  if (error_code_ == ErrorCode::kNone) {
    return;
  }
  const std::string_view name =
      error_code_ == ErrorCode::kNetworkError ? "NetworkError"
      : error_code_ == ErrorCode::kAbortError ? "AbortError"
                                              : "TimeoutError";
  exception_state.ThrowDOMException(
      name, base::StrCat({"Failed to load '", ElidedUrl(url_), "'",
                          reason.empty() ? "." : ": ", reason}));
}

void XMLHttpRequest::CreateRequest(std::optional<std::string> body,
                                   bindings::ExceptionState& exception_state) {
  // Only GET is supported for a blob: URL.
  if (url_.SchemeIsBlob() && method_ != "GET") {
    HandleNetworkError();
    if (!async_) {
      ThrowForLoadFailureIfNeeded(
          exception_state,
          "'GET' is the only method allowed for 'blob:' URLs.");
    }
    return;
  }
  if (url_.SchemeIs(url::kFtpScheme)) {
    GetExecutionContext()->AddConsoleMessage(ConsoleLevel::kError,
                                             "FTP is not supported.");
    HandleNetworkError();
    if (!async_) {
      ThrowForLoadFailureIfNeeded(
          exception_state, "Making a request to a FTP URL is not supported.");
    }
    return;
  }

  send_flag_ = true;
  ObserveContext();
  // Listeners of the upload force a preflight, because a POST to a URL that
  // permits no request of another origin has to look as one to a URL that does
  // not respond at all.
  bool upload_events = false;
  if (async_) {
    DispatchProgressEvent(u"loadstart", 0, 0);
    // A listener may have ended this send, or started another.
    if (!send_flag_ || loader_) {
      return;
    }
    if (body && upload_) {
      upload_events = upload_->HasEventListeners();
      upload_->DispatchLoadStart(body->size());
      if (!send_flag_ || loader_) {
        return;
      }
    }
  }
  if (!async_ && GetExecutionContext()->IsWindow() &&
      PageDismissalScope::IsActive()) {
    HandleNetworkError();
    ThrowForLoadFailureIfNeeded(
        exception_state,
        "Synchronous XHR in page dismissal. See "
        "https://www.chromestatus.com/feature/4664843055398912 for more "
        "details.");
    return;
  }
  StartLoader(std::move(body), upload_events);
  if (!async_) {
    ThrowForLoadFailureIfNeeded(exception_state, "");
  }
}

void XMLHttpRequest::StartLoader(std::optional<std::string> body,
                                 bool upload_events) {
  ExecutionContext* const context = GetExecutionContext();
  if (!context) {
    return;
  }
  // Whether upload events may fire, also for listeners that come later.
  upload_events_allowed_ =
      context->GetSecurityOrigin().IsSameOriginWith(url_) || upload_events ||
      !IsCorsSafelistedMethod(method_) ||
      !ContainsOnlyCorsSafelistedHeaders(request_headers_);

  reports_upload_ = async_ && upload_;
  error_code_ = ErrorCode::kNone;
  error_ = false;
  // Blink downloads the response of a request that asks for a blob to a blob,
  // but a data: URL's, which the renderer loads itself.
  downloading_to_blob_ = response_type_ == ResponseTypeCode::kBlob &&
                         !url_.SchemeIs(url::kDataScheme);
  loader_ = bindings::MakeGarbageCollected<XMLHttpRequestLoader>(this, context);
  loader_->SetTimeout(timeout_);
  // ResourceFetcher refuses a request while the window's document freezes,
  // with Chrome's console error, and it fails as a network error, from a task
  // that waits for the document to come back.
  if (context->IsWindow() &&
      static_cast<Window*>(context)->GetDocument().RefusesRequestWhileFreezing(
          url_)) {
    FailLoader();
    return;
  }
  if (Resource* const preload = MatchPreload()) {
    loader_->Start(*preload);
    return;
  }
  if (url_.SchemeIs(url::kDataScheme)) {
    if (async_) {
      loader_->Start(LocalSchemeFetch::AnswerDataUrl(url_, method_));
    } else {
      loader_->RunSynchronously(LocalSchemeFetch::DecodeDataUrl(url_, method_));
    }
    return;
  }
  if (url_.SchemeIsBlob()) {
    const std::string* const range = FindRequestHeader("range");
    const bool readable = blob_url_entry_
                              ? blob_url_entry_->origin.IsSameOriginWith(
                                    context->GetSecurityOrigin())
                              : url::Origin::Create(url_).IsSameOriginWith(
                                    context->GetSecurityOrigin());
    if (!readable) {
      FailLoader();
      return;
    }
    LocalSchemeFetch::Answer answer = LocalSchemeFetch::AnswerBlobUrl(
        url_, blob_url_entry_ ? blob_url_entry_->blob : nullptr, method_,
        range ? std::make_optional(*range) : std::nullopt);
    // The answer has what it needs of the blob.
    blob_url_entry_.reset();
    // BlobURLLoader's own, which fetch() has as "OK".
    if (answer.head.status == 206) {
      answer.head.status_text = "Partial Content";
    }
    if (async_) {
      loader_->Start(std::move(answer));
    } else {
      loader_->RunSynchronously(std::move(answer));
    }
    return;
  }
  Fetcher* const fetcher = context->GetFetcher();
  if (!fetcher || !url_.SchemeIsHTTPOrHTTPS()) {
    FailLoader();
    return;
  }
  FetchRequestData data;
  data.destination = FetchDestination::kXhr;
  data.method = method_;
  data.url = url_.spec();
  for (const auto& [name, value] : request_headers_) {
    base::StrAppend(&data.headers, {name, ": ", value, "\n"});
  }
  data.body = std::move(body);
  data.force_preflight = upload_events;
  data.report_upload_progress = reports_upload_;
  if (with_credentials_) {
    data.credentials = FetchCredentialsMode::kInclude;
  }
  // The document's policy, or that of the response of the worker's script.
  data.referrer_policy = context->GetReferrerPolicy();
  data.priority = ComputeFetchPriority(/*lazy=*/false, FetchPriorityHint::kAuto,
                                       /*render_blocking=*/false);
  data.initiator_type = u"xmlhttprequest";
  // It is the context's request, whichever realm's send() a script calls.
  RunInContext(context, [&] {
    if (async_) {
      loader_->Start(*fetcher, std::move(data));
    } else {
      // Blink gives a synchronous request the highest priority, as the Session
      // does.
      data.priority.reset();
      loader_->RunSynchronously(*fetcher, std::move(data));
    }
  });
}

Resource* XMLHttpRequest::MatchPreload() {
  // Blink downloads a blob: response to a blob, but a data: URL's, which no
  // preload has.
  if (!async_ || (response_type_ == ResponseTypeCode::kBlob &&
                  !url_.SchemeIs(url::kDataScheme))) {
    return nullptr;
  }
  return GetExecutionContext()->MatchFetchPreload(
      method_, url_, FetchMode::kCors,
      with_credentials_ ? FetchCredentialsMode::kInclude
                        : FetchCredentialsMode::kSameOrigin,
      [this](std::string_view name) {
        return FindRequestHeader(name) != nullptr;
      });
}

void XMLHttpRequest::FailLoader() {
  if (async_) {
    loader_->StartFailing(kFailed);
  } else {
    DidFail(kFailed);
  }
}

void XMLHttpRequest::Abort() {
  InternalAbort();
  // A synchronous request runs no script between send() and its end.
  if (async_ && ((state_ == kOpened && send_flag_) ||
                 state_ == kHeadersReceived || state_ == kLoading)) {
    DCHECK(!loader_);
    HandleRequestError(ErrorCode::kNone, u"abort");
  }
  if (state_ == kDone) {
    state_ = kUnsent;
  }
}

// How the request goes.

void XMLHttpRequest::AppendResponseText(const std::u16string& text) {
  // A string that a script could not have: none of it is kept, as in Blink.
  if (response_text_overflow_) {
    return;
  }
  if (text.size() >
      static_cast<size_t>(v8::String::kMaxLength) - response_text_.size()) {
    response_text_overflow_ = true;
    response_text_ = std::u16string();
    return;
  }
  response_text_ += text;
}

void XMLHttpRequest::TrackProgress(size_t length) {
  received_length_ += static_cast<int64_t>(length);
  ChangeState(kLoading);
  if (async_) {
    DispatchProgressEventFromSnapshot(u"progress");
  }
}

void XMLHttpRequest::ChangeState(State new_state) {
  if (state_ != new_state) {
    state_ = new_state;
    DispatchReadyStateChangeEvent();
  }
}

void XMLHttpRequest::DispatchReadyStateChangeEvent() {
  if (!GetExecutionContext()) {
    return;
  }
  using DeferredEventAction =
      XMLHttpRequestProgressEventThrottle::DeferredEventAction;
  if (async_ || state_ <= kOpened || state_ == kDone) {
    DeferredEventAction action = DeferredEventAction::kIgnore;
    if (state_ == kDone) {
      action =
          error_ ? DeferredEventAction::kClear : DeferredEventAction::kFlush;
    }
    progress_event_throttle_->DispatchReadyStateChangeEvent(action);
  }
  if (state_ == kDone && !error_) {
    DispatchProgressEventFromSnapshot(u"load");
    DispatchProgressEventFromSnapshot(u"loadend");
  }
}

void XMLHttpRequest::DispatchProgressEvent(const std::u16string& type,
                                           int64_t received_length,
                                           int64_t expected_length) {
  if (!CanDispatch()) {
    return;
  }
  const bool length_computable =
      expected_length > 0 && received_length <= expected_length;
  progress_event_throttle_->DispatchProgressEvent(
      type, length_computable,
      received_length >= 0 ? static_cast<uint64_t>(received_length) : 0,
      length_computable ? static_cast<uint64_t>(expected_length) : 0);
}

void XMLHttpRequest::DispatchProgressEventFromSnapshot(
    const std::u16string& type) {
  DispatchProgressEvent(type, received_length_, expected_length_);
}

void XMLHttpRequest::InternalAbort() {
  // The abort event that waits never fires.
  pending_abort_event_ = 0;
  // Repeated calls, as when the context goes.
  if (error_ && !loader_) {
    return;
  }
  error_ = true;
  ClearVariablesForLoading();
  ClearResponse();
  request_headers_.clear();
  StopObservingContext();
  if (XMLHttpRequestLoader* const loader = loader_.Get()) {
    loader_ = nullptr;
    loader->Cancel();
  }
}

void XMLHttpRequest::ClearVariablesForLoading() {
  if (BlobLoader* const blob_loader = blob_loader_.Get()) {
    blob_loader_ = nullptr;
    blob_loader->Cancel();
  }
  decoder_.reset();
}

void XMLHttpRequest::ClearResponse() {
  received_length_ = 0;
  expected_length_ = -1;
  response_.reset();
  response_headers_.clear();
  exposed_names_.clear();
  response_text_ = std::u16string();
  response_text_overflow_ = false;
  parsed_response_ = false;
  response_document_ = nullptr;
  response_blob_ = nullptr;
  downloading_to_blob_ = false;
  binary_response_ = std::string();
  response_array_buffer_.Reset();
  response_array_buffer_failure_ = false;
  ReportMemoryUsageToV8();
}

void XMLHttpRequest::HandleNetworkError() {
  InternalAbort();
  HandleRequestError(ErrorCode::kNetworkError, u"error");
}

void XMLHttpRequest::HandleDidCancel() {
  InternalAbort();
  ExecutionContext* const context = GetExecutionContext();
  if (!context) {
    return;
  }
  pending_abort_event_ = ++last_abort_event_;
  PostTaskInContext(
      context, TaskType::kNetworking,
      base::BindOnce(
          [](cppgc::Persistent<XMLHttpRequest> xhr, uint64_t event) {
            if (xhr->pending_abort_event_ == event) {
              xhr->pending_abort_event_ = 0;
              xhr->HandleRequestError(ErrorCode::kAbortError, u"abort");
            }
          },
          cppgc::Persistent<XMLHttpRequest>(this), pending_abort_event_));
}

void XMLHttpRequest::HandleDidTimeout() {
  InternalAbort();
  HandleRequestError(ErrorCode::kTimeoutError, u"timeout");
}

void XMLHttpRequest::HandleRequestError(ErrorCode code,
                                        const std::u16string& type) {
  send_flag_ = false;
  if (!async_) {
    DCHECK(code != ErrorCode::kNone);
    state_ = kDone;
    error_code_ = code;
    return;
  }
  DCHECK(error_);
  ChangeState(kDone);
  if (!upload_complete_) {
    upload_complete_ = true;
    if (upload_ && upload_events_allowed_) {
      upload_->HandleRequestError(type);
    }
  }
  DispatchProgressEvent(type, 0, 0);
  DispatchProgressEvent(u"loadend", 0, 0);
}

void XMLHttpRequest::DidSendData(uint64_t bytes_sent,
                                 uint64_t total_bytes_to_be_sent) {
  if (!upload_ || !reports_upload_) {
    return;
  }
  if (upload_events_allowed_) {
    upload_->DispatchProgressEvent(bytes_sent, total_bytes_to_be_sent);
  }
  if (bytes_sent == total_bytes_to_be_sent && !upload_complete_) {
    upload_complete_ = true;
    if (upload_events_allowed_) {
      upload_->DispatchEventAndLoadEnd(u"load", true, bytes_sent,
                                       total_bytes_to_be_sent);
    }
  }
}

void XMLHttpRequest::DidReceiveResponse(const FetchResponseHead& head) {
  // ThreadableLoader::ResponseReceived(): a response with Cache-Control:
  // no-store keeps a no-store page of the context out of the back/forward
  // cache.
  if (CacheControlContainsNoStore(HeaderField(head.headers, "cache-control"))) {
    if (ExecutionContext* const context = GetExecutionContext()) {
      context->DidReceiveNoStoreForScriptRequest();
    }
  }
  response_ = head;
  response_headers_.clear();
  // Each name once, with its values joined, as HTTPHeaderMap keeps them.
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
    const auto [place, is_first] =
        places.emplace(base::ToLowerASCII(name), response_headers_.size());
    if (is_first) {
      response_headers_.emplace_back(name, value);
    } else {
      base::StrAppend(&response_headers_[place->second].second, {", ", value});
    }
  }
  // ExpectedContentLength(): the bytes of a data: URL, and otherwise what
  // Content-Length says, unless the body is decoded, of which URLRequestJob
  // expects no size.
  if (head.expected_length) {
    expected_length_ = static_cast<int64_t>(*head.expected_length);
  } else if (!IsDecodedBody(ResponseHeader("content-encoding").value_or(""))) {
    expected_length_ =
        ContentLength(ResponseHeader("content-length").value_or(""));
  } else {
    expected_length_ = -1;
  }
  exposed_names_ =
      head.cors
          ? CorsExposedHeaderNames(
                ResponseHeader("access-control-expose-headers").value_or(""))
          : std::set<std::string>();
}

void XMLHttpRequest::DidReceiveData(std::string_view data) {
  if (error_) {
    return;
  }
  if (state_ < kHeadersReceived) {
    ChangeState(kHeadersReceived);
  }
  // A listener may have aborted the request.
  if (error_ || data.empty()) {
    return;
  }
  // What comes as a blob stays bytes until it has all come, whatever
  // responseType becomes, as Blink's DidDownloadData() counts it.
  if (downloading_to_blob_) {
    binary_response_.append(data);
    ReportMemoryUsageToV8();
    TrackProgress(data.size());
    return;
  }
  switch (response_type_) {
    case ResponseTypeCode::kDefault:
    case ResponseTypeCode::kText:
    case ResponseTypeCode::kJson:
    case ResponseTypeCode::kDocument:
      if (!decoder_) {
        decoder_ = CreateDecoder();
        // Blink's ParseDocumentChunk() makes an HTML document as its first
        // bytes come, of the response as it is then.
        if (response_type_ == ResponseTypeCode::kDocument && ResponseIsHtml()) {
          InitResponseDocument();
        }
      }
      AppendResponseText(decoder_->Decode(base::as_byte_span(data)));
      break;
    case ResponseTypeCode::kArrayBuffer:
    case ResponseTypeCode::kBlob:
      binary_response_.append(data);
      break;
  }
  ReportMemoryUsageToV8();
  // What a blob's read gives back has had its progress events.
  if (blob_loader_) {
    return;
  }
  TrackProgress(data.size());
}

void XMLHttpRequest::DidFinishLoading() {
  if (error_) {
    return;
  }
  // As in Blink, the end goes on whatever a listener does, such as one that
  // aborts the request or opens it again.
  if (state_ < kHeadersReceived) {
    ChangeState(kHeadersReceived);
  }
  // A response that came as a blob, which responseType no longer asks for, is
  // read back first.
  if (downloading_to_blob_ && response_type_ != ResponseTypeCode::kBlob &&
      !error_) {
    downloading_to_blob_ = false;
    blob_loader_ = bindings::MakeGarbageCollected<BlobLoader>(
        this, BlobDataHandle::Create(std::exchange(binary_response_, {}), ""));
    return;
  }
  DidFinishLoadingInternal();
}

void XMLHttpRequest::DidFinishLoadingFromBlob() {
  DidFinishLoadingInternal();
}

void XMLHttpRequest::DidFailLoadingFromBlob() {
  if (error_) {
    return;
  }
  HandleNetworkError();
}

void XMLHttpRequest::DidFinishLoadingInternal() {
  if (decoder_) {
    AppendResponseText(decoder_->Flush());
    ReportMemoryUsageToV8();
    // An HTML document is parsed as it comes, so it is there before DONE.
    if (response_type_ == ResponseTypeCode::kDocument && ResponseIsHtml()) {
      ParseResponseDocument();
    }
  }
  ClearVariablesForLoading();
  EndLoading();
}

void XMLHttpRequest::DidFail(int net_error) {
  if (error_) {
    return;
  }
  if (net_error == kAborted) {
    HandleDidCancel();
    return;
  }
  if (net_error == kTimedOut) {
    HandleDidTimeout();
    return;
  }
  HandleNetworkError();
}

void XMLHttpRequest::EndLoading() {
  // One that a listener of this request's end has started goes too.
  if (XMLHttpRequestLoader* const loader = loader_.Get()) {
    loader_ = nullptr;
    loader->Cancel();
  }
  StopObservingContext();
  send_flag_ = false;
  ChangeState(kDone);
}

void XMLHttpRequest::ObserveContext() {
  DCHECK(!context_destroyed_callback_);
  context_destroyed_callback_ =
      GetExecutionContext()->AddContextDestroyedCallback(base::BindOnce(
          [](cppgc::WeakPersistent<XMLHttpRequest> xhr) {
            if (xhr) {
              xhr->ContextDestroyed();
            }
          },
          cppgc::WeakPersistent<XMLHttpRequest>(this)));
}

void XMLHttpRequest::StopObservingContext() {
  if (ExecutionContext* const context = context_.Get();
      context && context_destroyed_callback_) {
    context->RemoveContextDestroyedCallback(context_destroyed_callback_);
  }
  context_destroyed_callback_ = 0;
}

void XMLHttpRequest::CheckContext() {
  if (!GetExecutionContext()) {
    InternalAbort();
  }
}

void XMLHttpRequest::ContextDestroyed() {
  context_destroyed_callback_ = 0;
  progress_event_throttle_->Stop();
  InternalAbort();
  // Within send(), the flag ends it.
  send_flag_ = false;
}

// The response's type and text.

std::string XMLHttpRequest::FinalResponseMimeType() const {
  if (const std::optional<std::string> overridden =
          ExtractMimeTypeFromMediaType(mime_type_override_,
                                       /*accept_comma_separated=*/false)) {
    return *overridden;
  }
  if (!response_) {
    return std::string();
  }
  const std::string content_type = ResponseHeader("content-type").value_or("");
  if (GURL(response_->url).SchemeIsHTTPOrHTTPS()) {
    return ExtractMimeTypeFromMediaType(content_type,
                                        /*accept_comma_separated=*/true)
        .value_or(std::string());
  }
  return GetMimeTypeAndCharset(content_type).mime_type;
}

std::string XMLHttpRequest::ResponseMimeType() const {
  const std::string final_type = FinalResponseMimeType();
  return final_type.empty() ? "text/xml" : final_type;
}

std::string XMLHttpRequest::FinalResponseCharset() const {
  std::string label =
      response_
          ? GetMimeTypeAndCharset(ResponseHeader("content-type").value_or(""))
                .charset
          : std::string();
  const std::string overridden =
      ExtractCharsetFromMediaType(mime_type_override_);
  if (!overridden.empty()) {
    label = overridden;
  }
  return CanonicalTextEncodingName(label);
}

bool XMLHttpRequest::ResponseIsXml() const {
  return IsXmlMimeType(ResponseMimeType());
}

bool XMLHttpRequest::ResponseIsHtml() const {
  return base::EqualsCaseInsensitiveASCII(FinalResponseMimeType(), "text/html");
}

std::unique_ptr<TextResourceDecoder> XMLHttpRequest::CreateDecoder() const {
  using Options = TextResourceDecoderOptions;
  using ContentType = Options::ContentType;
  if (response_type_ == ResponseTypeCode::kJson) {
    return std::make_unique<TextResourceDecoder>(Options::CreateUtf8Decode());
  }
  // A charset that is given is not looked for in the content.
  if (const std::string charset = FinalResponseCharset(); !charset.empty()) {
    return std::make_unique<TextResourceDecoder>(
        Options(ContentType::kPlainText, charset));
  }
  Options xml(ContentType::kXml, "");
  // Bytes that the encoding cannot decode do not stop it.
  xml.use_lenient_xml_decoding = true;
  switch (response_type_) {
    case ResponseTypeCode::kDefault:
      if (ResponseIsXml()) {
        return std::make_unique<TextResourceDecoder>(xml);
      }
      [[fallthrough]];
    case ResponseTypeCode::kText:
      return std::make_unique<TextResourceDecoder>(
          Options(ContentType::kPlainText, "UTF-8"));
    case ResponseTypeCode::kDocument:
      if (ResponseIsHtml()) {
        return std::make_unique<TextResourceDecoder>(
            Options(ContentType::kHtml, "UTF-8"));
      }
      return std::make_unique<TextResourceDecoder>(xml);
    case ResponseTypeCode::kJson:
    case ResponseTypeCode::kBlob:
    case ResponseTypeCode::kArrayBuffer:
      break;
  }
  NOTREACHED();
}

void XMLHttpRequest::ReportMemoryUsageToV8() {
  const int64_t size = static_cast<int64_t>(
      binary_response_.size() + response_text_.size() * sizeof(char16_t));
  if (size != reported_memory_) {
    memory_accounter_.Update(v8::Isolate::GetCurrent(),
                             size - reported_memory_);
    reported_memory_ = size;
  }
}

}  // namespace mitto
