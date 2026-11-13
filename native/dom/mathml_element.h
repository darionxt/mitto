// The DOM's MathMLElement: every element of the MathML namespace, whatever its
// name, as Chrome gives them all this interface. Blink's subclasses of it,
// such as MathMLRowElement, hold what layout needs, and MathMLAnchorElement
// is behind a feature that is off.
//
// Upstream: third_party/blink/renderer/core/mathml/mathml_element.h
// Upstream: third_party/blink/renderer/core/mathml/mathml_tag_names.json5

#ifndef MITTO_DOM_MATHML_ELEMENT_H_
#define MITTO_DOM_MATHML_ELEMENT_H_

#include <string>

#include "mitto/dom/element.h"

namespace mitto {

class MathMLElement : public Element {
 public:
  static MathMLElement* Create(Document& document,
                               const std::u16string& local_name);

  MathMLElement(Document& document, const std::u16string& local_name);
  ~MathMLElement() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_MATHML_ELEMENT_H_
