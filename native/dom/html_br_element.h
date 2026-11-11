// The br element.
//
// Upstream: third_party/blink/renderer/core/html/html_br_element.h
// Upstream: third_party/blink/renderer/core/html/html_br_element.cc

#ifndef MITTO_DOM_HTML_BR_ELEMENT_H_
#define MITTO_DOM_HTML_BR_ELEMENT_H_

#include <string>

#include "mitto/dom/html_element.h"

namespace mitto {

class HTMLBRElement final : public HTMLElement {
 public:
  static HTMLBRElement* Create(Document& document);

  explicit HTMLBRElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_BR_ELEMENT_H_
