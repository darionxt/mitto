// The ol element.
//
// Upstream: third_party/blink/renderer/core/html/html_olist_element.h
// Upstream: third_party/blink/renderer/core/html/html_olist_element.cc

#ifndef MITTO_DOM_HTML_OLIST_ELEMENT_H_
#define MITTO_DOM_HTML_OLIST_ELEMENT_H_

#include <string>

#include "mitto/dom/html_element.h"

namespace mitto {

class HTMLOListElement final : public HTMLElement {
 public:
  static HTMLOListElement* Create(Document& document);

  explicit HTMLOListElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_OLIST_ELEMENT_H_
