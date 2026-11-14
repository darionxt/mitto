// The body element.
//
// Upstream: third_party/blink/renderer/core/html/html_body_element.h
// Upstream: third_party/blink/renderer/core/html/html_body_element.cc

#ifndef MITTO_DOM_HTML_BODY_ELEMENT_H_
#define MITTO_DOM_HTML_BODY_ELEMENT_H_

#include <string>
#include <string_view>

#include "mitto/dom/html_element.h"

namespace mitto {

class HTMLBodyElement final : public HTMLElement {
 public:
  static HTMLBodyElement* Create(Document& document);

  explicit HTMLBodyElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

 protected:
  // HTMLElement's: the window's handlers of Blink's
  // HTMLBodyElement::ParseAttribute().
  std::u16string_view WindowEventNameForAttributeName(
      const QualifiedName& name) const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_BODY_ELEMENT_H_
