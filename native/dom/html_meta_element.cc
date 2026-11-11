#include "mitto/dom/html_meta_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_meta_element.h"

namespace mitto {

// static
HTMLMetaElement* HTMLMetaElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLMetaElement>(document);
}

HTMLMetaElement::HTMLMetaElement(Document& document)
    : HTMLElement(document, u"meta") {}

const bindings::WrapperTypeInfo* HTMLMetaElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLMetaElement::kWrapperTypeInfo;
}

}  // namespace mitto
