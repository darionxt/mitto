// The DOM's SVGElement: an element of the SVG namespace, and the class of
// those whose name SVG does not know, as Blink's SVGUnknownElement is one. It
// keeps the animated values that a page asked for of its attributes.
//
// Upstream: third_party/blink/renderer/core/svg/svg_element.h
// Upstream: third_party/blink/renderer/core/svg/svg_unknown_element.h

#ifndef MITTO_DOM_SVG_ELEMENT_H_
#define MITTO_DOM_SVG_ELEMENT_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>

#include "mitto/dom/element.h"
#include "mitto/dom/heap_vector.h"
#include "mitto/dom/svg_animated_values.h"
#include "mitto/dom/svg_geometry.h"
#include "mitto/dom/svg_length.h"
#include "mitto/dom/svg_lists.h"
#include "mitto/dom/svg_transforms.h"

namespace mitto {

class SVGSVGElement;

class SVGElement : public Element {
 public:
  static SVGElement* Create(Document& document,
                            const std::u16string& local_name);

  SVGElement(Document& document, const std::u16string& local_name);
  ~SVGElement() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface, and of SVGURIReference and
  // SVGFilterPrimitiveStandardAttributes, which the interfaces that include
  // them reach here.
  SVGAnimatedString* ClassName();
  SVGAnimatedString* Href();
  SVGAnimatedString* Result();
  // SVGTests', which the interfaces that include it reach here.
  SVGStringList* RequiredExtensions();
  SVGStringList* SystemLanguage();
  // SVGFitToViewBox's, and preserveAspectRatio of image and feImage too.
  SVGAnimatedRect* ViewBox();
  SVGAnimatedPreserveAspectRatio* PreserveAspectRatio();
  // The nearest svg ancestor, but none for an outermost svg, and the nearest
  // svg, image or symbol ancestor, as Blink's ownerSVGElement() and
  // viewportElement() walk up.
  SVGSVGElement* OwnerSVGElement() const;
  SVGElement* ViewportElement() const;

  // Blink's SVG load event: once the element and the SVG elements it holds
  // have their resources, load fires at it if it is structurally external or
  // an svg and something listens, and SendSVGLoadEventToSelfAndAncestorChain-
  // IfPossible() goes on to its SVG ancestors until the document's load event
  // has finished, but never to an outermost svg, whose load is the
  // document's. Whether it had its resources.
  bool SendSVGLoadEventIfPossible();
  void SendSVGLoadEventToSelfAndAncestorChainIfPossible();
  // Whether the resources of the element and of its SVG children have come.
  virtual bool HaveLoadedRequiredResources();

  // Writes back the value that a page set of an animated value, which the
  // value keeps rather than reading again.
  // A null value removes the attribute.
  void SynchronizeAnimatedAttribute(const QualifiedName& name,
                                    const std::optional<std::u16string>& value);

  // Element:
  void AttributeChanged(const QualifiedName& name,
                        const std::u16string* old_value,
                        const std::u16string* new_value) override;
  // Node's: the nonce hides as the element goes into a tree.
  InsertionNotificationRequest InsertedInto(Node& insertion_point) override;

 protected:
  // Blink's SvgAttributeChanged(): `name` changed, by the DOM, or by a page
  // that set an animated value's baseVal, which writes it back.
  virtual void SvgAttributeChanged(const QualifiedName& name) {}
  // Whether the element names a resource of its own, as an image with an
  // href does.
  virtual bool IsStructurallyExternal() const;

  // Element's: the copy's event handler content attributes set handlers, as
  // Blink's CloneAttributesFrom() runs them through AttributeChanged().
  void DidCloneAttributes() override;

  // The same object for the attribute on every read, made the first time, as
  // Blink makes its own with the element.
  SVGAnimatedString* AnimatedString(std::u16string_view attribute);
  SVGAnimatedBoolean* AnimatedBoolean(std::u16string_view attribute);
  SVGAnimatedEnumeration* AnimatedEnumeration(std::u16string_view attribute,
                                              const SVGEnumerationMap& map,
                                              uint16_t initial_value);
  SVGAnimatedInteger* AnimatedInteger(std::u16string_view attribute,
                                      int32_t initial_value);
  // A half of feConvolveMatrix's order.
  SVGAnimatedInteger* AnimatedOrder(SVGPairHalf half);
  SVGAnimatedNumber* AnimatedNumber(
      std::u16string_view attribute,
      float initial_value,
      SVGAnimatedNumber::Syntax syntax = SVGAnimatedNumber::Syntax::kNumber);
  // A half of an attribute of one number or two, such as stdDeviation.
  SVGAnimatedNumber* AnimatedNumberHalf(std::u16string_view attribute,
                                        SVGPairHalf half,
                                        float initial_value);
  SVGAnimatedNumberList* AnimatedNumberList(std::u16string_view attribute);
  SVGAnimatedLengthList* AnimatedLengthList(std::u16string_view attribute,
                                            SVGLengthMode mode);
  SVGAnimatedLength* AnimatedLength(std::u16string_view attribute,
                                    SVGLengthMode mode,
                                    SVGAnimatedLength::Initial initial,
                                    bool text_length = false);
  // A marker's orient.
  SVGAnimatedAngle* AnimatedOrient();
  // A polyline's or polygon's points.
  SVGAnimatedPointList* AnimatedPointList();
  SVGAnimatedTransformList* AnimatedTransformList(
      std::u16string_view attribute);

 private:
  SVGAnimatedPropertyBase* FindAnimated(std::u16string_view attribute,
                                        SVGPairHalf half) const;

  HeapVector<SVGAnimatedPropertyBase> animated_values_;
  // An animated value writes its attribute back, which it need not read.
  bool synchronizing_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_SVG_ELEMENT_H_
