#include "mitto/dom/html_unknown_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_unknown_element.h"

namespace mitto {

// static
HTMLUnknownElement* HTMLUnknownElement::Create(
    Document& document,
    const std::u16string& local_name) {
  return bindings::MakeGarbageCollected<HTMLUnknownElement>(document,
                                                            local_name);
}

HTMLUnknownElement::HTMLUnknownElement(Document& document,
                                       const std::u16string& local_name)
    : HTMLElement(document, local_name) {}

const bindings::WrapperTypeInfo* HTMLUnknownElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLUnknownElement::kWrapperTypeInfo;
}

}  // namespace mitto
