// The XMLHttpRequest Standard's XMLHttpRequest, as Blink's: a request through
// the Session that loaded the document, or for a data: or a blob: URL, whose
// response comes as text, JSON, a document, a Blob or an ArrayBuffer, with
// Blink's events in Blink's order. A synchronous one blocks its thread until
// its end.
//
// Blink's ThreadableLoader is XMLHttpRequestLoader here, which holds the
// Fetcher's side of the request.
//
// Upstream: third_party/blink/renderer/core/xmlhttprequest/xml_http_request.h
// Upstream: third_party/blink/renderer/core/xmlhttprequest/xml_http_request.cc

#ifndef MITTO_DOM_XML_HTTP_REQUEST_H_
#define MITTO_DOM_XML_HTTP_REQUEST_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/blob_data.h"
#include "mitto/dom/blob_url_registry.h"
#include "mitto/dom/fetcher.h"
#include "mitto/dom/xml_http_request_event_target.h"
#include "mitto/encoding/text_resource_decoder.h"
#include "url/gurl.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-external-memory-accounter.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-traced-handle.h"

namespace v8 {
class ArrayBuffer;
class Value;
}  // namespace v8

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class Blob;
class Document;
class ExecutionContext;
class Resource;
class XMLHttpRequestLoader;
class XMLHttpRequestProgressEventThrottle;
class XMLHttpRequestUpload;

class XMLHttpRequest final : public XMLHttpRequestEventTarget {
 public:
  class BlobLoader;

  enum State {
    kUnsent = 0,
    kOpened = 1,
    kHeadersReceived = 2,
    kLoading = 3,
    kDone = 4,
  };

  // `new XMLHttpRequest()`, of the current context.
  static XMLHttpRequest* Create();

  explicit XMLHttpRequest(ExecutionContext* context);
  ~XMLHttpRequest() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  uint16_t ReadyState() const { return static_cast<uint16_t>(state_); }
  void Open(const std::string& method,
            const std::u16string& url,
            bindings::ExceptionState& exception_state);
  void Open(const std::string& method,
            const std::u16string& url,
            bool async,
            const std::optional<std::u16string>& username,
            const std::optional<std::u16string>& password,
            bindings::ExceptionState& exception_state);
  void SetRequestHeader(const std::string& name,
                        const std::string& value,
                        bindings::ExceptionState& exception_state);
  uint32_t Timeout() const {
    return static_cast<uint32_t>(timeout_.InMilliseconds());
  }
  void SetTimeout(uint32_t timeout, bindings::ExceptionState& exception_state);
  bool WithCredentials() const { return with_credentials_; }
  void SetWithCredentials(bool value,
                          bindings::ExceptionState& exception_state);
  XMLHttpRequestUpload* Upload();
  void Send(
      const std::optional<
          bindings::
              DocumentOrBlobOrArrayBufferOrArrayBufferViewOrFormDataOrURLSearchParamsOrUSVString>&
          body,
      bindings::ExceptionState& exception_state);
  void Abort();
  std::u16string ResponseURL();
  uint16_t Status();
  std::string StatusText();
  std::optional<std::string> GetResponseHeader(const std::string& name);
  std::string GetAllResponseHeaders();
  void OverrideMimeType(const std::u16string& mime_type,
                        bindings::ExceptionState& exception_state);
  std::u16string ResponseType() const;
  void SetResponseType(const std::u16string& response_type,
                       bindings::ExceptionState& exception_state);
  v8::Local<v8::Value> Response();
  std::u16string ResponseText(bindings::ExceptionState& exception_state);
  Document* ResponseXML(bindings::ExceptionState& exception_state);

  State state() const { return state_; }
  // Null once the context has gone, unless it had gone before the request was
  // made, as Blink's observer of a context that is destroyed never hears of
  // it.
  ExecutionContext* GetExecutionContext() const;
  // Fires a readystatechange, with no check.
  // EventTarget::FireEventListeners(): whether the context is there, without
  // which no listener hears of anything.
  bool CanDispatch() const;
  void DispatchReadyStateChange();

  // What the loader tells.
  void DidSendData(uint64_t bytes_sent, uint64_t total_bytes_to_be_sent);
  void DidReceiveResponse(const FetchResponseHead& head);
  void DidReceiveData(std::string_view data);
  void DidFinishLoading();
  // What the read of a response that came as a blob tells, once responseType
  // has changed since send().
  void DidFinishLoadingFromBlob();
  void DidFailLoadingFromBlob();
  // `net_error` is why the request failed.
  void DidFail(int net_error);
  // Blink's ContextDestroyed(), which the context calls while a request runs.
  // One that has ended, of which a page may make any number, hears of it when
  // a script next reads its response, in CheckContext().
  void ContextDestroyed();
  void ObserveContext();
  void StopObservingContext();
  void CheckContext();
  // Adds `text` to what responseText gives.
  void AppendResponseText(const std::u16string& text);

 private:
  enum class ResponseTypeCode {
    kDefault,
    kArrayBuffer,
    kBlob,
    kDocument,
    kJson,
    kText,
  };
  // The DOMException that a synchronous request throws.
  enum class ErrorCode { kNone, kNetworkError, kAbortError, kTimeoutError };

  void OpenUrl(const std::string& method,
               const GURL& url,
               bool async,
               bindings::ExceptionState& exception_state);
  bool InitSend(bindings::ExceptionState& exception_state);
  bool AreMethodAndURLValidForSend() const;
  void CreateRequest(std::optional<std::string> body,
                     bindings::ExceptionState& exception_state);
  // Starts the request of `body` that CreateRequest() has checked, or runs a
  // synchronous one to its end.
  void StartLoader(std::optional<std::string> body, bool upload_events);
  // What a link preloaded that the request takes, if any.
  Resource* MatchPreload();
  // Fails a request that goes nowhere, from a task or now.
  void FailLoader();
  void ThrowForLoadFailureIfNeeded(bindings::ExceptionState& exception_state,
                                   std::string_view reason);

  bool HasContentTypeRequestHeader() const;
  std::string* FindRequestHeader(std::string_view name);
  void SetRequestHeaderInternal(std::string_view name, std::string_view value);
  void UpdateContentTypeAndCharset(std::string_view default_content_type,
                                   std::string_view charset);

  void TrackProgress(size_t length);
  // The end of a load whose body has all come, and been read back if it came
  // as a blob.
  void DidFinishLoadingInternal();
  void ChangeState(State new_state);
  void DispatchReadyStateChangeEvent();
  void DispatchProgressEvent(const std::u16string& type,
                             int64_t received_length,
                             int64_t expected_length);
  void DispatchProgressEventFromSnapshot(const std::u16string& type);

  void InternalAbort();
  void ClearVariablesForLoading();
  void ClearResponse();
  void HandleNetworkError();
  void HandleDidCancel();
  void HandleDidTimeout();
  void HandleRequestError(ErrorCode code, const std::u16string& type);
  void EndLoading();
  std::optional<std::string> ResponseHeader(std::string_view name) const;
  // Names that a script may read of a response of another origin.
  bool IsExposed(std::string_view name) const;
  std::string FinalResponseMimeType() const;
  std::string ResponseMimeType() const;
  std::string FinalResponseCharset() const;
  bool ResponseIsXml() const;
  bool ResponseIsHtml() const;
  std::unique_ptr<TextResourceDecoder> CreateDecoder() const;
  void InitResponseDocument();
  // Parses the response's text into the response's document, or null.
  void ParseResponseDocument();
  v8::Local<v8::Value> ResponseJson() const;
  Blob* ResponseBlob();
  v8::Local<v8::Value> ResponseArrayBuffer();
  void ReportMemoryUsageToV8();

  cppgc::WeakMember<ExecutionContext> context_;
  const cppgc::Member<XMLHttpRequestProgressEventThrottle>
      progress_event_throttle_;
  const bool made_without_context_;
  cppgc::Member<XMLHttpRequestUpload> upload_;
  cppgc::Member<XMLHttpRequestLoader> loader_;
  // What reads a response that came as a blob back, once it has all come.
  cppgc::Member<BlobLoader> blob_loader_;

  GURL url_;
  // What a blob: URL named when open() ran.
  std::optional<BlobUrlRegistry::Entry> blob_url_entry_;
  std::string method_;
  std::vector<std::pair<std::string, std::string>> request_headers_;
  std::string mime_type_override_;
  base::TimeDelta timeout_;
  // The abort event that waits for its task, by its number, or 0.
  uint64_t pending_abort_event_ = 0;
  uint64_t last_abort_event_ = 0;
  // What tells that the context goes, or 0.
  uint64_t context_destroyed_callback_ = 0;

  State state_ = kUnsent;
  ResponseTypeCode response_type_ = ResponseTypeCode::kDefault;
  ErrorCode error_code_ = ErrorCode::kNone;

  std::optional<FetchResponseHead> response_;
  // The response's lines, without Set-Cookie.
  std::vector<std::pair<std::string, std::string>> response_headers_;
  // The names that the response of another origin exposes, in lower case.
  std::set<std::string> exposed_names_;
  int64_t received_length_ = 0;
  // Of the response's body, or -1.
  int64_t expected_length_ = -1;
  std::unique_ptr<TextResourceDecoder> decoder_;
  std::u16string response_text_;
  // Whether it grew past what a string can hold.
  bool response_text_overflow_ = false;
  std::string binary_response_;
  cppgc::Member<Document> response_document_;
  cppgc::Member<Blob> response_blob_;
  v8::TracedReference<v8::ArrayBuffer> response_array_buffer_;
  // What V8 has been told that the response keeps.
  int64_t reported_memory_ = 0;
  v8::ExternalMemoryAccounter memory_accounter_;

  bool async_ = true;
  bool with_credentials_ = false;
  bool send_flag_ = false;
  bool error_ = false;
  bool parsed_response_ = false;
  bool response_array_buffer_failure_ = false;
  bool upload_events_allowed_ = true;
  bool upload_complete_ = false;
  // Whether the response comes as a blob, which send() asked for, whatever
  // responseType becomes since, as Blink downloads it to a blob.
  bool downloading_to_blob_ = false;
  // Whether upload was read before send(), so that the request reports how its
  // body goes.
  bool reports_upload_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_XML_HTTP_REQUEST_H_
