#include "mitto/dom/attr.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_attr.h"
#include "mitto/dom/document.h"
#include "mitto/dom/element.h"

namespace mitto {

// static
Attr* Attr::Create(Element& element, const QualifiedName& name) {
  return bindings::MakeGarbageCollected<Attr>(element, name);
}

// static
Attr* Attr::Create(Document& document,
                   const QualifiedName& name,
                   const std::u16string& value) {
  return bindings::MakeGarbageCollected<Attr>(document, name, value);
}

Attr::Attr(Element& element, const QualifiedName& name)
    : Node(&element.GetDocument()), element_(&element), name_(name) {}

Attr::Attr(Document& document,
           const QualifiedName& name,
           const std::u16string& value)
    : Node(&document), name_(name), standalone_value_(value) {}

const bindings::WrapperTypeInfo* Attr::GetWrapperTypeInfo() const {
  return &bindings::V8Attr::kWrapperTypeInfo;
}

void Attr::Trace(cppgc::Visitor* visitor) const {
  Node::Trace(visitor);
  visitor->Trace(element_);
}

std::u16string Attr::Value() const {
  if (element_) {
    const std::u16string* value = element_->FindAttribute(name_);
    return value ? *value : std::u16string();
  }
  return standalone_value_;
}

void Attr::SetValue(const std::u16string& value,
                    bindings::ExceptionState& exception_state) {
  // Blink's Element::SetAttributeWithValidation(), with no trusted types to
  // check.
  if (element_) {
    element_->SetAttributeWithoutValidation(name_, value);
    return;
  }
  standalone_value_ = value;
}

uint16_t Attr::GetNodeType() const {
  return kAttributeNode;
}

std::u16string Attr::NodeName() const {
  return Name();
}

std::optional<std::u16string> Attr::NodeValue() const {
  return Value();
}

void Attr::SetNodeValue(const std::optional<std::u16string>& value,
                        bindings::ExceptionState& exception_state) {
  SetValue(value.value_or(std::u16string()), exception_state);
}

std::optional<std::u16string> Attr::TextContentForBinding() const {
  return Value();
}

void Attr::SetTextContentForBinding(const std::optional<std::u16string>& value,
                                    bindings::ExceptionState& exception_state) {
  SetNodeValue(value, exception_state);
}

void Attr::AttachToElement(Element& element) {
  element_ = &element;
}

void Attr::DetachFromElementWithValue(const std::u16string& value) {
  standalone_value_ = value;
  element_ = nullptr;
}

Node* Attr::CloneWithoutChildren(Document& document) const {
  return Create(document, name_, Value());
}

}  // namespace mitto
