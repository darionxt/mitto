// The XMLHttpRequest Standard's XMLHttpRequestEventTarget, which
// XMLHttpRequest and XMLHttpRequestUpload are.
//
// Upstream: third_party/blink/renderer/core/xmlhttprequest/xml_http_request_event_target.h

#ifndef MITTO_DOM_XML_HTTP_REQUEST_EVENT_TARGET_H_
#define MITTO_DOM_XML_HTTP_REQUEST_EVENT_TARGET_H_

#include "mitto/dom/event_target.h"

namespace mitto {

class XMLHttpRequestEventTarget : public EventTarget {
 public:
  ~XMLHttpRequestEventTarget() override;

 protected:
  XMLHttpRequestEventTarget();
};

}  // namespace mitto

#endif  // MITTO_DOM_XML_HTTP_REQUEST_EVENT_TARGET_H_
