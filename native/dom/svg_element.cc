#include "mitto/dom/svg_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_svg_element.h"
#include "mitto/dom/html_names.h"

namespace mitto {

// static
SVGElement* SVGElement::Create(Document& document,
                               const std::u16string& local_name) {
  return bindings::MakeGarbageCollected<SVGElement>(document, local_name);
}

SVGElement::SVGElement(Document& document, const std::u16string& local_name)
    : Element(document, html_names::kSvgNamespace, std::nullopt, local_name) {}

SVGElement::~SVGElement() = default;

const bindings::WrapperTypeInfo* SVGElement::GetWrapperTypeInfo() const {
  return &bindings::V8SVGElement::kWrapperTypeInfo;
}

}  // namespace mitto
