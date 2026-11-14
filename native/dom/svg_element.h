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

namespace mitto {

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

  // Writes back the value that a page set of an animated value, which the
  // value keeps rather than reading again.
  // A null value removes the attribute.
  void SynchronizeAnimatedAttribute(const QualifiedName& name,
                                    const std::optional<std::u16string>& value);

  // Element:
  void AttributeChanged(const QualifiedName& name,
                        const std::u16string* old_value,
                        const std::u16string* new_value) override;

 protected:
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

 private:
  SVGAnimatedPropertyBase* FindAnimated(std::u16string_view attribute,
                                        SVGPairHalf half) const;

  HeapVector<SVGAnimatedPropertyBase> animated_values_;
  // An animated value writes its attribute back, which it need not read.
  bool synchronizing_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_SVG_ELEMENT_H_
