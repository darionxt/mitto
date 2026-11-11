// The p element.
//
// Upstream: third_party/blink/renderer/core/html/html_paragraph_element.h
// Upstream: third_party/blink/renderer/core/html/html_paragraph_element.cc

#ifndef MITTO_DOM_HTML_PARAGRAPH_ELEMENT_H_
#define MITTO_DOM_HTML_PARAGRAPH_ELEMENT_H_

#include <string>

#include "mitto/dom/html_element.h"

namespace mitto {

class HTMLParagraphElement final : public HTMLElement {
 public:
  static HTMLParagraphElement* Create(Document& document);

  explicit HTMLParagraphElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_PARAGRAPH_ELEMENT_H_
