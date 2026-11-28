// The XMLHttpRequest Standard's XMLHttpRequestUpload: what an XMLHttpRequest
// tells of how its body goes, as Blink's.
//
// Upstream: third_party/blink/renderer/core/xmlhttprequest/xml_http_request_upload.h
// Upstream: third_party/blink/renderer/core/xmlhttprequest/xml_http_request_upload.cc

#ifndef MITTO_DOM_XML_HTTP_REQUEST_UPLOAD_H_
#define MITTO_DOM_XML_HTTP_REQUEST_UPLOAD_H_

#include <stdint.h>

#include <string>

#include "mitto/dom/xml_http_request_event_target.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class XMLHttpRequest;

class XMLHttpRequestUpload final : public XMLHttpRequestEventTarget {
 public:
  explicit XMLHttpRequestUpload(XMLHttpRequest* xml_http_request);
  ~XMLHttpRequestUpload() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  void DispatchLoadStart(uint64_t total);
  void DispatchProgressEvent(uint64_t bytes_sent,
                             uint64_t total_bytes_to_be_sent);
  // `type`, load, abort, error or timeout, and then loadend.
  void DispatchEventAndLoadEnd(const std::u16string& type,
                               bool length_computable,
                               uint64_t bytes_sent,
                               uint64_t total);
  // `type` and then loadend, with how far the body had gone.
  void HandleRequestError(const std::u16string& type);

 private:
  void Dispatch(const std::u16string& type,
                bool length_computable,
                uint64_t bytes_sent,
                uint64_t total);

  const cppgc::Member<XMLHttpRequest> xml_http_request_;
  // The last update.
  uint64_t last_bytes_sent_ = 0;
  uint64_t last_total_bytes_to_be_sent_ = 0;
};

}  // namespace mitto

#endif  // MITTO_DOM_XML_HTTP_REQUEST_UPLOAD_H_
