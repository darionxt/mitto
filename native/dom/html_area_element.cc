#include "mitto/dom/html_area_element.h"

#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_area_element.h"
#include "mitto/dom/document.h"
#include "mitto/dom/html_parser_idioms.h"
#include "url/gurl.h"

namespace mitto {

// static
HTMLAreaElement* HTMLAreaElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLAreaElement>(document);
}

HTMLAreaElement::HTMLAreaElement(Document& document)
    : HTMLElement(document, u"area") {}

const bindings::WrapperTypeInfo* HTMLAreaElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLAreaElement::kWrapperTypeInfo;
}

std::u16string HTMLAreaElement::Href() const {
  // Blink's DOMURLUtilsReadOnly::href(): the attribute as it is, spaces and
  // all, when it does not resolve.
  const std::u16string* value = FindAttribute(u"href");
  if (!value) {
    return std::u16string();
  }
  const GURL url = GetDocument().CompleteURL(StripHTMLSpaces(*value));
  return url.is_valid() ? base::UTF8ToUTF16(url.spec()) : *value;
}

void HTMLAreaElement::SetHref(const std::u16string& value) {
  SetAttributeWithoutValidation(u"href", value);
}

}  // namespace mitto
