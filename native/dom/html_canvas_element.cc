#include "mitto/dom/html_canvas_element.h"

#include "base/strings/string_number_conversions.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_canvas_element.h"
#include "mitto/dom/html_parser_idioms.h"

namespace mitto {

namespace {

constexpr uint32_t kDefaultWidth = 300;
constexpr uint32_t kDefaultHeight = 150;

}  // namespace

// static
HTMLCanvasElement* HTMLCanvasElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLCanvasElement>(document);
}

HTMLCanvasElement::HTMLCanvasElement(Document& document)
    : HTMLElement(document, u"canvas") {}

const bindings::WrapperTypeInfo* HTMLCanvasElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLCanvasElement::kWrapperTypeInfo;
}

uint32_t HTMLCanvasElement::Width() const {
  const std::u16string* value = FindAttribute(u"width");
  return value ? ParseHTMLNonNegativeInteger(*value).value_or(kDefaultWidth)
               : kDefaultWidth;
}

void HTMLCanvasElement::SetWidth(uint32_t value,
                                 bindings::ExceptionState& exception_state) {
  // Blink's SetUnsignedIntegralAttribute() with the canvas's default.
  SetAttributeWithoutValidation(
      u"width",
      base::NumberToString16(value > 0x7fffffffu ? kDefaultWidth : value));
}

uint32_t HTMLCanvasElement::Height() const {
  const std::u16string* value = FindAttribute(u"height");
  return value ? ParseHTMLNonNegativeInteger(*value).value_or(kDefaultHeight)
               : kDefaultHeight;
}

void HTMLCanvasElement::SetHeight(uint32_t value,
                                  bindings::ExceptionState& exception_state) {
  SetAttributeWithoutValidation(
      u"height",
      base::NumberToString16(value > 0x7fffffffu ? kDefaultHeight : value));
}

}  // namespace mitto
