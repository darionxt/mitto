#include "mitto/dom/html_pre_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_pre_element.h"

namespace mitto {

// static
HTMLPreElement* HTMLPreElement::Create(Document& document,
                                       const std::u16string& local_name) {
  return bindings::MakeGarbageCollected<HTMLPreElement>(document, local_name);
}

HTMLPreElement::HTMLPreElement(Document& document,
                               const std::u16string& local_name)
    : HTMLElement(document, local_name) {}

const bindings::WrapperTypeInfo* HTMLPreElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLPreElement::kWrapperTypeInfo;
}

}  // namespace mitto
