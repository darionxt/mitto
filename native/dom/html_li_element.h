// The li element.
//
// Upstream: third_party/blink/renderer/core/html/html_li_element.h
// Upstream: third_party/blink/renderer/core/html/html_li_element.cc

#ifndef MITTO_DOM_HTML_LI_ELEMENT_H_
#define MITTO_DOM_HTML_LI_ELEMENT_H_

#include <string>

#include "mitto/dom/html_element.h"

namespace mitto {

class HTMLLIElement final : public HTMLElement {
 public:
  static HTMLLIElement* Create(Document& document);

  explicit HTMLLIElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_LI_ELEMENT_H_
