// The title element, whose text is the document's title.
//
// Upstream: third_party/blink/renderer/core/html/html_title_element.h
// Upstream: third_party/blink/renderer/core/html/html_title_element.cc

#ifndef MITTO_DOM_HTML_TITLE_ELEMENT_H_
#define MITTO_DOM_HTML_TITLE_ELEMENT_H_

#include <string>

#include "mitto/dom/html_element.h"

namespace mitto {

class HTMLTitleElement final : public HTMLElement {
 public:
  static HTMLTitleElement* Create(Document& document);

  explicit HTMLTitleElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  std::u16string Text() const { return ChildTextContent(); }
  void SetText(const std::u16string& text);
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_TITLE_ELEMENT_H_
