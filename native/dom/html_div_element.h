// The div element.
//
// Upstream: third_party/blink/renderer/core/html/html_div_element.h
// Upstream: third_party/blink/renderer/core/html/html_div_element.cc

#ifndef MITTO_DOM_HTML_DIV_ELEMENT_H_
#define MITTO_DOM_HTML_DIV_ELEMENT_H_

#include <string>

#include "mitto/dom/html_element.h"

namespace mitto {

class HTMLDivElement final : public HTMLElement {
 public:
  static HTMLDivElement* Create(Document& document);

  explicit HTMLDivElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_DIV_ELEMENT_H_
