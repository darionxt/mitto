#include "mitto/dom/html_li_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_li_element.h"

namespace mitto {

// static
HTMLLIElement* HTMLLIElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLLIElement>(document);
}

HTMLLIElement::HTMLLIElement(Document& document)
    : HTMLElement(document, u"li") {}

const bindings::WrapperTypeInfo* HTMLLIElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLLIElement::kWrapperTypeInfo;
}

}  // namespace mitto
