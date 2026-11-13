#include "mitto/dom/html_element.h"

#include "base/strings/string_util.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_element.h"
#include "mitto/dom/html_names.h"

namespace mitto {

// static
HTMLElement* HTMLElement::Create(Document& document,
                                 const std::u16string& local_name) {
  return bindings::MakeGarbageCollected<HTMLElement>(document, local_name);
}

HTMLElement::HTMLElement(Document& document, const std::u16string& local_name)
    : Element(document, html_names::kXhtmlNamespace, std::nullopt, local_name) {
}

HTMLElement::~HTMLElement() = default;

const bindings::WrapperTypeInfo* HTMLElement::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLElement::kWrapperTypeInfo;
}

std::u16string HTMLElement::Autocapitalize() const {
  const std::u16string value = ReflectedStringAttribute(u"autocapitalize");
  if (value.empty()) {
    return std::u16string();
  }
  if (base::EqualsCaseInsensitiveASCII(value, u"none") ||
      base::EqualsCaseInsensitiveASCII(value, u"off")) {
    return u"none";
  }
  if (base::EqualsCaseInsensitiveASCII(value, u"characters")) {
    return u"characters";
  }
  if (base::EqualsCaseInsensitiveASCII(value, u"words")) {
    return u"words";
  }
  // "sentences", "on", or any other value.
  return u"sentences";
}

void HTMLElement::SetAutocapitalize(const std::u16string& value) {
  SetReflectedStringAttribute(u"autocapitalize", value);
}

}  // namespace mitto
