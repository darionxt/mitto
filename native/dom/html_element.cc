#include "mitto/dom/html_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_element.h"
#include "mitto/dom/html_names.h"

namespace mitto {

// static
HTMLElement* HTMLElement::Create(Document& document,
                                 const std::u16string& local_name) {
  return bindings::MakeGarbageCollected<HTMLElement>(document, local_name);
}

HTMLElement::HTMLElement(Document& document, const std::u16string& local_name)
    : Element(document, html_names::kXhtmlNamespace, std::nullopt, local_name) {
}

HTMLElement::~HTMLElement() = default;

const bindings::WrapperTypeInfo* HTMLElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLElement::kWrapperTypeInfo;
}

}  // namespace mitto
