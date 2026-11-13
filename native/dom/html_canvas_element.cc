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
uint32_t HTMLCanvasElement::Dimension(const Element& element,
                                      std::u16string_view name,
                                      uint32_t default_value) {
  // Blink's Reset(): an attribute that is not a non-negative integer of at
  // most 2147483647 gives the default.
  const std::u16string* value = element.FindAttribute(name);
  const std::optional<uint32_t> parsed =
      value ? ParseHTMLNonNegativeInteger(*value) : std::nullopt;
  return parsed && *parsed <= 0x7fffffffu ? *parsed : default_value;
}

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
  return Dimension(*this, u"width", kDefaultWidth);
}

void HTMLCanvasElement::SetWidth(uint32_t value,
                                 bindings::ExceptionState& exception_state) {
  // Blink's SetUnsignedIntegralAttribute() with the canvas's default.
  SetAttributeWithoutValidation(
      u"width",
      base::NumberToString16(value > 0x7fffffffu ? kDefaultWidth : value));
}

uint32_t HTMLCanvasElement::Height() const {
  return Dimension(*this, u"height", kDefaultHeight);
}

void HTMLCanvasElement::SetHeight(uint32_t value,
                                  bindings::ExceptionState& exception_state) {
  SetAttributeWithoutValidation(
      u"height",
      base::NumberToString16(value > 0x7fffffffu ? kDefaultHeight : value));
}

}  // namespace mitto
