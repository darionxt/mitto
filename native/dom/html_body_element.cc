#include "mitto/dom/html_body_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_body_element.h"
#include "mitto/dom/event_handler_attributes.h"

namespace mitto {

// static
HTMLBodyElement* HTMLBodyElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLBodyElement>(document);
}

HTMLBodyElement::HTMLBodyElement(Document& document)
    : HTMLElement(document, u"body") {}

const bindings::WrapperTypeInfo* HTMLBodyElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLBodyElement::kWrapperTypeInfo;
}

std::u16string_view HTMLBodyElement::WindowEventNameForAttributeName(
    const QualifiedName& name) const {
  return BodyWindowEventNameForAttributeName(name);
}

}  // namespace mitto
