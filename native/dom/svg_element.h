// The DOM's SVGElement: an element of the SVG namespace, and the class of
// those whose name SVG does not know, as Blink's SVGUnknownElement is one.
//
// Upstream: third_party/blink/renderer/core/svg/svg_element.h
// Upstream: third_party/blink/renderer/core/svg/svg_unknown_element.h

#ifndef MITTO_DOM_SVG_ELEMENT_H_
#define MITTO_DOM_SVG_ELEMENT_H_

#include <string>

#include "mitto/dom/element.h"

namespace mitto {

class SVGElement : public Element {
 public:
  static SVGElement* Create(Document& document,
                            const std::u16string& local_name);

  SVGElement(Document& document, const std::u16string& local_name);
  ~SVGElement() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_SVG_ELEMENT_H_
