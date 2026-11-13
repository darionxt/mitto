#include "mitto/dom/html_area_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_area_element.h"

namespace mitto {

// static
HTMLAreaElement* HTMLAreaElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLAreaElement>(document);
}

HTMLAreaElement::HTMLAreaElement(Document& document)
    : HTMLAnchorElementBase(document, u"area") {}

const bindings::WrapperTypeInfo* HTMLAreaElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLAreaElement::kWrapperTypeInfo;
}

}  // namespace mitto
