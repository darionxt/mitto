#include "mitto/dom/xml_http_request_upload.h"

#include "mitto/bindings/v8_xml_http_request_upload.h"
#include "mitto/dom/progress_event.h"
#include "mitto/dom/xml_http_request.h"

namespace mitto {

XMLHttpRequestUpload::XMLHttpRequestUpload(XMLHttpRequest* xml_http_request)
    : xml_http_request_(xml_http_request) {}

XMLHttpRequestUpload::~XMLHttpRequestUpload() = default;

const bindings::WrapperTypeInfo* XMLHttpRequestUpload::GetWrapperTypeInfo()
    const {
  return &bindings::V8XMLHttpRequestUpload::kWrapperTypeInfo;
}

void XMLHttpRequestUpload::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(xml_http_request_);
  XMLHttpRequestEventTarget::Trace(visitor);
}

void XMLHttpRequestUpload::DispatchLoadStart(uint64_t total) {
  Dispatch(u"loadstart", true, 0, total);
}

void XMLHttpRequestUpload::DispatchProgressEvent(
    uint64_t bytes_sent,
    uint64_t total_bytes_to_be_sent) {
  last_bytes_sent_ = bytes_sent;
  last_total_bytes_to_be_sent_ = total_bytes_to_be_sent;
  Dispatch(u"progress", true, bytes_sent, total_bytes_to_be_sent);
}

void XMLHttpRequestUpload::DispatchEventAndLoadEnd(const std::u16string& type,
                                                   bool length_computable,
                                                   uint64_t bytes_sent,
                                                   uint64_t total) {
  Dispatch(type, length_computable, bytes_sent, total);
  Dispatch(u"loadend", length_computable, bytes_sent, total);
}

void XMLHttpRequestUpload::Dispatch(const std::u16string& type,
                                    bool length_computable,
                                    uint64_t bytes_sent,
                                    uint64_t total) {
  if (xml_http_request_->CanDispatch()) {
    DispatchEvent(ProgressEvent::CreateTrusted(type, length_computable,
                                               bytes_sent, total));
  }
}

void XMLHttpRequestUpload::HandleRequestError(const std::u16string& type) {
  const bool length_computable =
      last_total_bytes_to_be_sent_ > 0 &&
      last_bytes_sent_ <= last_total_bytes_to_be_sent_;
  DispatchEventAndLoadEnd(type, length_computable, last_bytes_sent_,
                          last_total_bytes_to_be_sent_);
}

}  // namespace mitto
