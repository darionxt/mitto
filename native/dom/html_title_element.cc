#include "mitto/dom/html_title_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_title_element.h"
#include "mitto/dom/text.h"

namespace mitto {

// static
HTMLTitleElement* HTMLTitleElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLTitleElement>(document);
}

HTMLTitleElement::HTMLTitleElement(Document& document)
    : HTMLElement(document, u"title") {}

const bindings::WrapperTypeInfo* HTMLTitleElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLTitleElement::kWrapperTypeInfo;
}

void HTMLTitleElement::SetText(const std::u16string& text) {
  // The DOM standard's "string replace all".
  RemoveChildren();
  if (!text.empty()) {
    ParserAppendChild(mitto::Text::Create(GetDocument(), text));
  }
}

}  // namespace mitto
