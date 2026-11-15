#include "mitto/dom/html_olist_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_olist_element.h"
#include "mitto/dom/html_parser_idioms.h"

namespace mitto {

// static
HTMLOListElement* HTMLOListElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLOListElement>(document);
}

HTMLOListElement::HTMLOListElement(Document& document)
    : HTMLElement(document, u"ol") {}

const bindings::WrapperTypeInfo* HTMLOListElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLOListElement::kWrapperTypeInfo;
}

int32_t HTMLOListElement::Start() const {
  const std::u16string* const start = FindAttribute(u"start");
  return start ? ParseHTMLInteger(*start).value_or(1) : 1;
}

void HTMLOListElement::SetStart(int32_t start) {
  SetReflectedIntegerAttribute(u"start", start);
}

}  // namespace mitto
