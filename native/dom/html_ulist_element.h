// The ul element.
//
// Upstream: third_party/blink/renderer/core/html/html_ulist_element.h
// Upstream: third_party/blink/renderer/core/html/html_ulist_element.cc

#ifndef MITTO_DOM_HTML_ULIST_ELEMENT_H_
#define MITTO_DOM_HTML_ULIST_ELEMENT_H_

#include <string>

#include "mitto/dom/html_element.h"

namespace mitto {

class HTMLUListElement final : public HTMLElement {
 public:
  static HTMLUListElement* Create(Document& document);

  explicit HTMLUListElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_ULIST_ELEMENT_H_
