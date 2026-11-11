#include "mitto/dom/html_heading_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_heading_element.h"

namespace mitto {

// static
HTMLHeadingElement* HTMLHeadingElement::Create(
    Document& document,
    const std::u16string& local_name) {
  return bindings::MakeGarbageCollected<HTMLHeadingElement>(document,
                                                            local_name);
}

HTMLHeadingElement::HTMLHeadingElement(Document& document,
                                       const std::u16string& local_name)
    : HTMLElement(document, local_name) {}

const bindings::WrapperTypeInfo* HTMLHeadingElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLHeadingElement::kWrapperTypeInfo;
}

}  // namespace mitto
