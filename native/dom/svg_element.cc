#include "mitto/dom/svg_element.h"

#include "base/auto_reset.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_svg_element.h"
#include "mitto/dom/event_handler_attributes.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/svg_elements.h"

namespace mitto {

// static
SVGElement* SVGElement::Create(Document& document,
                               const std::u16string& local_name) {
  return bindings::MakeGarbageCollected<SVGElement>(document, local_name);
}

SVGElement::SVGElement(Document& document, const std::u16string& local_name)
    : Element(document, html_names::kSvgNamespace, std::nullopt, local_name) {}

SVGElement::~SVGElement() = default;

Node::InsertionNotificationRequest SVGElement::InsertedInto(
    Node& insertion_point) {
  const InsertionNotificationRequest request =
      Element::InsertedInto(insertion_point);
  HideNonce();
  return request;
}

const bindings::WrapperTypeInfo* SVGElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGElement::kWrapperTypeInfo;
}

void SVGElement::Trace(cppgc::Visitor* visitor) const {
  Element::Trace(visitor);
  animated_values_.Trace(visitor);
}

namespace {

bool IsSVGElement(const Node* node) {
  return node && node->IsElementNode() &&
         static_cast<const Element*>(node)->TagQName().namespace_uri ==
             html_names::kSvgNamespace;
}

bool IsSVGElementNamed(const Node* node, std::u16string_view local_name) {
  return IsSVGElement(node) &&
         static_cast<const Element*>(node)->LocalName() == local_name;
}

// Blink's IsOutermostSVGSVGElement(), with no use shadow trees: an svg with
// no parent, or a parent that is a foreignObject or no SVG element.
bool IsOutermostSVGSVGElement(const SVGElement& element) {
  if (element.LocalName() != u"svg") {
    return false;
  }
  const Node* const parent = element.ParentNode();
  return !parent || IsSVGElementNamed(parent, u"foreignObject") ||
         !IsSVGElement(parent);
}

}  // namespace

SVGSVGElement* SVGElement::OwnerSVGElement() const {
  if (IsOutermostSVGSVGElement(*this)) {
    return nullptr;
  }
  for (Node* node = ParentNode(); node; node = node->ParentNode()) {
    if (IsSVGElementNamed(node, u"svg")) {
      return static_cast<SVGSVGElement*>(node);
    }
  }
  return nullptr;
}

SVGElement* SVGElement::ViewportElement() const {
  for (Node* node = ParentNode(); node; node = node->ParentNode()) {
    if (IsSVGElementNamed(node, u"svg") || IsSVGElementNamed(node, u"image") ||
        IsSVGElementNamed(node, u"symbol")) {
      return static_cast<SVGElement*>(node);
    }
  }
  return nullptr;
}

SVGAnimatedString* SVGElement::ClassName() {
  return AnimatedString(u"class");
}

SVGAnimatedString* SVGElement::Href() {
  return AnimatedString(u"href");
}

SVGAnimatedString* SVGElement::Result() {
  return AnimatedString(u"result");
}

SVGStringList* SVGElement::RequiredExtensions() {
  if (SVGAnimatedPropertyBase* const value =
          FindAnimated(u"requiredExtensions", SVGPairHalf::kNone)) {
    return static_cast<SVGStringList*>(value);
  }
  SVGStringList* const value =
      SVGStringList::Create(*this, u"requiredExtensions", u' ');
  animated_values_.push_back(value);
  return value;
}

SVGStringList* SVGElement::SystemLanguage() {
  if (SVGAnimatedPropertyBase* const value =
          FindAnimated(u"systemLanguage", SVGPairHalf::kNone)) {
    return static_cast<SVGStringList*>(value);
  }
  SVGStringList* const value =
      SVGStringList::Create(*this, u"systemLanguage", u',');
  animated_values_.push_back(value);
  return value;
}

SVGAnimatedRect* SVGElement::ViewBox() {
  if (SVGAnimatedPropertyBase* const value =
          FindAnimated(u"viewBox", SVGPairHalf::kNone)) {
    return static_cast<SVGAnimatedRect*>(value);
  }
  SVGAnimatedRect* const value = SVGAnimatedRect::Create(*this);
  animated_values_.push_back(value);
  return value;
}

SVGAnimatedPreserveAspectRatio* SVGElement::PreserveAspectRatio() {
  if (SVGAnimatedPropertyBase* const value =
          FindAnimated(u"preserveAspectRatio", SVGPairHalf::kNone)) {
    return static_cast<SVGAnimatedPreserveAspectRatio*>(value);
  }
  SVGAnimatedPreserveAspectRatio* const value =
      SVGAnimatedPreserveAspectRatio::Create(*this);
  animated_values_.push_back(value);
  return value;
}

SVGAnimatedAngle* SVGElement::AnimatedOrient() {
  if (SVGAnimatedPropertyBase* const value =
          FindAnimated(u"orient", SVGPairHalf::kNone)) {
    return static_cast<SVGAnimatedAngle*>(value);
  }
  SVGAnimatedAngle* const value = SVGAnimatedAngle::Create(*this);
  animated_values_.push_back(value);
  return value;
}

SVGAnimatedPointList* SVGElement::AnimatedPointList() {
  if (SVGAnimatedPropertyBase* const value =
          FindAnimated(u"points", SVGPairHalf::kNone)) {
    return static_cast<SVGAnimatedPointList*>(value);
  }
  SVGAnimatedPointList* const value = SVGAnimatedPointList::Create(*this);
  animated_values_.push_back(value);
  return value;
}

SVGAnimatedTransformList* SVGElement::AnimatedTransformList(
    std::u16string_view attribute) {
  if (SVGAnimatedPropertyBase* const value =
          FindAnimated(attribute, SVGPairHalf::kNone)) {
    return static_cast<SVGAnimatedTransformList*>(value);
  }
  SVGAnimatedTransformList* const value =
      SVGAnimatedTransformList::Create(*this, attribute);
  animated_values_.push_back(value);
  return value;
}

SVGAnimatedNumberList* SVGElement::AnimatedNumberList(
    std::u16string_view attribute) {
  if (SVGAnimatedPropertyBase* const value =
          FindAnimated(attribute, SVGPairHalf::kNone)) {
    return static_cast<SVGAnimatedNumberList*>(value);
  }
  SVGAnimatedNumberList* const value =
      SVGAnimatedNumberList::Create(*this, attribute);
  animated_values_.push_back(value);
  return value;
}

SVGAnimatedLengthList* SVGElement::AnimatedLengthList(
    std::u16string_view attribute,
    SVGLengthMode mode) {
  if (SVGAnimatedPropertyBase* const value =
          FindAnimated(attribute, SVGPairHalf::kNone)) {
    return static_cast<SVGAnimatedLengthList*>(value);
  }
  SVGAnimatedLengthList* const value =
      SVGAnimatedLengthList::Create(*this, attribute, mode);
  animated_values_.push_back(value);
  return value;
}

void SVGElement::SynchronizeAnimatedAttribute(
    const QualifiedName& name,
    const std::optional<std::u16string>& value) {
  const base::AutoReset<bool> synchronizing(&synchronizing_, true);
  if (value) {
    SetSynchronizedLazyAttribute(name, *value);
  } else {
    RemoveSynchronizedLazyAttribute(name);
  }
}

void SVGElement::DidCloneAttributes() {
  for (const Attribute& attribute : attributes()) {
    if (const std::u16string_view event =
            EventNameForAttributeName(attribute.name);
        !event.empty()) {
      SetElementAttributeEventListenerFromScriptBody(
          *this, event, attribute.name, &attribute.value);
    }
  }
}

void SVGElement::AttributeChanged(const QualifiedName& name,
                                  const std::u16string* old_value,
                                  const std::u16string* new_value) {
  Element::AttributeChanged(name, old_value, new_value);
  // Blink's SVGElement::ParseAttribute(): the event handler content
  // attributes of HTML elements.
  if (const std::u16string_view event = EventNameForAttributeName(name);
      !event.empty()) {
    SetElementAttributeEventListenerFromScriptBody(*this, event, name,
                                                   new_value);
    return;
  }
  if (synchronizing_) {
    return;
  }
  for (size_t i = 0; i < animated_values_.size(); ++i) {
    if (animated_values_[i]->ReadsAttribute(name)) {
      animated_values_[i]->AttributeChanged();
    }
  }
}

SVGAnimatedPropertyBase* SVGElement::FindAnimated(std::u16string_view attribute,
                                                  SVGPairHalf half) const {
  for (size_t i = 0; i < animated_values_.size(); ++i) {
    SVGAnimatedPropertyBase* const value = animated_values_[i];
    if (value->AttributeName() == attribute && value->Half() == half) {
      return value;
    }
  }
  return nullptr;
}

SVGAnimatedString* SVGElement::AnimatedString(std::u16string_view attribute) {
  if (SVGAnimatedPropertyBase* const value =
          FindAnimated(attribute, SVGPairHalf::kNone)) {
    return static_cast<SVGAnimatedString*>(value);
  }
  SVGAnimatedString* const value = SVGAnimatedString::Create(*this, attribute);
  animated_values_.push_back(value);
  return value;
}

SVGAnimatedBoolean* SVGElement::AnimatedBoolean(std::u16string_view attribute) {
  if (SVGAnimatedPropertyBase* const value =
          FindAnimated(attribute, SVGPairHalf::kNone)) {
    return static_cast<SVGAnimatedBoolean*>(value);
  }
  SVGAnimatedBoolean* const value =
      SVGAnimatedBoolean::Create(*this, attribute);
  animated_values_.push_back(value);
  return value;
}

SVGAnimatedEnumeration* SVGElement::AnimatedEnumeration(
    std::u16string_view attribute,
    const SVGEnumerationMap& map,
    uint16_t initial_value) {
  if (SVGAnimatedPropertyBase* const value =
          FindAnimated(attribute, SVGPairHalf::kNone)) {
    return static_cast<SVGAnimatedEnumeration*>(value);
  }
  SVGAnimatedEnumeration* const value =
      SVGAnimatedEnumeration::Create(*this, attribute, map, initial_value);
  animated_values_.push_back(value);
  return value;
}

SVGAnimatedInteger* SVGElement::AnimatedInteger(std::u16string_view attribute,
                                                int32_t initial_value) {
  if (SVGAnimatedPropertyBase* const value =
          FindAnimated(attribute, SVGPairHalf::kNone)) {
    return static_cast<SVGAnimatedInteger*>(value);
  }
  SVGAnimatedInteger* const value =
      SVGAnimatedInteger::Create(*this, attribute, initial_value);
  animated_values_.push_back(value);
  return value;
}

SVGAnimatedInteger* SVGElement::AnimatedOrder(SVGPairHalf half) {
  if (SVGAnimatedPropertyBase* const value = FindAnimated(u"order", half)) {
    return static_cast<SVGAnimatedInteger*>(value);
  }
  // Blink's SVGAnimatedOrder: 3 by 3 until the attribute says otherwise.
  constexpr int32_t kInitialOrder = 3;
  SVGAnimatedInteger* const first = SVGAnimatedInteger::CreateOrderHalf(
      *this, SVGPairHalf::kFirst, kInitialOrder);
  SVGAnimatedInteger* const second = SVGAnimatedInteger::CreateOrderHalf(
      *this, SVGPairHalf::kSecond, kInitialOrder);
  first->SetOtherHalf(*second);
  second->SetOtherHalf(*first);
  animated_values_.push_back(first);
  animated_values_.push_back(second);
  return half == SVGPairHalf::kFirst ? first : second;
}

SVGAnimatedNumber* SVGElement::AnimatedNumber(
    std::u16string_view attribute,
    float initial_value,
    SVGAnimatedNumber::Syntax syntax) {
  if (SVGAnimatedPropertyBase* const value =
          FindAnimated(attribute, SVGPairHalf::kNone)) {
    return static_cast<SVGAnimatedNumber*>(value);
  }
  SVGAnimatedNumber* const value =
      SVGAnimatedNumber::Create(*this, attribute, initial_value, syntax);
  animated_values_.push_back(value);
  return value;
}

SVGAnimatedNumber* SVGElement::AnimatedNumberHalf(std::u16string_view attribute,
                                                  SVGPairHalf half,
                                                  float initial_value) {
  if (SVGAnimatedPropertyBase* const value = FindAnimated(attribute, half)) {
    return static_cast<SVGAnimatedNumber*>(value);
  }
  SVGAnimatedNumber* const first = SVGAnimatedNumber::CreateHalf(
      *this, attribute, SVGPairHalf::kFirst, initial_value);
  SVGAnimatedNumber* const second = SVGAnimatedNumber::CreateHalf(
      *this, attribute, SVGPairHalf::kSecond, initial_value);
  first->SetOtherHalf(*second);
  second->SetOtherHalf(*first);
  animated_values_.push_back(first);
  animated_values_.push_back(second);
  return half == SVGPairHalf::kFirst ? first : second;
}

SVGAnimatedLength* SVGElement::AnimatedLength(
    std::u16string_view attribute,
    SVGLengthMode mode,
    SVGAnimatedLength::Initial initial,
    bool text_length) {
  if (SVGAnimatedPropertyBase* const value =
          FindAnimated(attribute, SVGPairHalf::kNone)) {
    return static_cast<SVGAnimatedLength*>(value);
  }
  SVGAnimatedLength* const value =
      SVGAnimatedLength::Create(*this, attribute, mode, initial, text_length);
  animated_values_.push_back(value);
  return value;
}

}  // namespace mitto
