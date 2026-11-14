#include "mitto/dom/mathml_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_mathml_element.h"
#include "mitto/dom/event_handler_attributes.h"
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

void MathMLElement::AttributeChanged(const QualifiedName& name,
                                     const std::u16string* old_value,
                                     const std::u16string* new_value) {
  Element::AttributeChanged(name, old_value, new_value);
  if (const std::u16string_view event = EventNameForAttributeName(name);
      !event.empty()) {
    SetElementAttributeEventListenerFromScriptBody(*this, event, name,
                                                   new_value);
  }
}

}  // namespace mitto
