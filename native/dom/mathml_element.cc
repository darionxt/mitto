#include "mitto/dom/mathml_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_mathml_element.h"
#include "mitto/dom/html_names.h"

namespace mitto {

// static
MathMLElement* MathMLElement::Create(Document& document,
                                     const std::u16string& local_name) {
  return bindings::MakeGarbageCollected<MathMLElement>(document, local_name);
}

MathMLElement::MathMLElement(Document& document,
                             const std::u16string& local_name)
    : Element(document,
              html_names::kMathMlNamespace,
              std::nullopt,
              local_name) {}

MathMLElement::~MathMLElement() = default;

const bindings::WrapperTypeInfo* MathMLElement::GetWrapperTypeInfo() const {
  return &bindings::V8MathMLElement::kWrapperTypeInfo;
}

}  // namespace mitto
