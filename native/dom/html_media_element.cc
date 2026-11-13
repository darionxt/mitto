#include "mitto/dom/html_media_element.h"

#include "base/strings/string_util.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_media_element.h"

namespace mitto {

HTMLMediaElement::HTMLMediaElement(Document& document,
                                   const std::u16string& local_name)
    : HTMLElement(document, local_name) {}

const bindings::WrapperTypeInfo* HTMLMediaElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLMediaElement::kWrapperTypeInfo;
}

std::u16string HTMLMediaElement::Preload() const {
  const std::u16string* value = FindAttribute(u"preload");
  if (value && base::EqualsCaseInsensitiveASCII(*value, u"none")) {
    return u"none";
  }
  if (value && base::EqualsCaseInsensitiveASCII(*value, u"metadata")) {
    return u"metadata";
  }
  if (value &&
      (value->empty() || base::EqualsCaseInsensitiveASCII(*value, u"auto"))) {
    return u"auto";
  }
  return u"metadata";
}

void HTMLMediaElement::SetPreload(const std::u16string& value) {
  SetAttributeWithoutValidation(u"preload", value);
}

}  // namespace mitto
