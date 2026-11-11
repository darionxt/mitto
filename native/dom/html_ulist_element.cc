#include "mitto/dom/html_ulist_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_ulist_element.h"

namespace mitto {

// static
HTMLUListElement* HTMLUListElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLUListElement>(document);
}

HTMLUListElement::HTMLUListElement(Document& document)
    : HTMLElement(document, u"ul") {}

const bindings::WrapperTypeInfo* HTMLUListElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLUListElement::kWrapperTypeInfo;
}

}  // namespace mitto
