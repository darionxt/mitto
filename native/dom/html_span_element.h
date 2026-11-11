// The span element.
//
// Upstream: third_party/blink/renderer/core/html/html_span_element.h
// Upstream: third_party/blink/renderer/core/html/html_span_element.cc

#ifndef MITTO_DOM_HTML_SPAN_ELEMENT_H_
#define MITTO_DOM_HTML_SPAN_ELEMENT_H_

#include <string>

#include "mitto/dom/html_element.h"

namespace mitto {

class HTMLSpanElement final : public HTMLElement {
 public:
  static HTMLSpanElement* Create(Document& document);

  explicit HTMLSpanElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_SPAN_ELEMENT_H_
