// The html element, a document's root.
//
// Upstream: third_party/blink/renderer/core/html/html_html_element.h
// Upstream: third_party/blink/renderer/core/html/html_html_element.cc

#ifndef MITTO_DOM_HTML_HTML_ELEMENT_H_
#define MITTO_DOM_HTML_HTML_ELEMENT_H_

#include <string>

#include "mitto/dom/html_element.h"

namespace mitto {

class HTMLHtmlElement final : public HTMLElement {
 public:
  static HTMLHtmlElement* Create(Document& document);

  explicit HTMLHtmlElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_HTML_ELEMENT_H_
