#include "mitto/dom/html_element.h"

#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_element.h"
#include "mitto/dom/document.h"
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

namespace {

// Blink's contentEditableNormalized(): "true", "false", "plaintext-only",
// or "inherit" for a missing or another value.
std::u16string_view ContentEditableState(const Element& element) {
  const std::u16string* value = element.FindAttribute(u"contenteditable");
  if (!value) {
    return u"inherit";
  }
  if (value->empty() || base::EqualsCaseInsensitiveASCII(*value, u"true")) {
    return u"true";
  }
  if (base::EqualsCaseInsensitiveASCII(*value, u"false")) {
    return u"false";
  }
  if (base::EqualsCaseInsensitiveASCII(*value, u"plaintext-only")) {
    return u"plaintext-only";
  }
  return u"inherit";
}

}  // namespace

int32_t HTMLElement::DefaultTabIndex() const {
  if (!HasHTMLTagName(u"summary")) {
    return -1;
  }
  Element* const parent = ParentElement();
  if (!parent || !parent->HasHTMLTagName(u"details")) {
    return -1;
  }
  for (Element* child = parent->FirstElementChild(); child;
       child = child->NextElementSibling()) {
    if (child->HasHTMLTagName(u"summary")) {
      return child == this ? 0 : -1;
    }
  }
  return -1;
}

std::u16string HTMLElement::Dir() const {
  const std::u16string* value = FindAttribute(u"dir");
  if (value) {
    for (const std::u16string_view keyword : {u"ltr", u"rtl", u"auto"}) {
      if (base::EqualsCaseInsensitiveASCII(*value, keyword)) {
        return std::u16string(keyword);
      }
    }
  }
  return std::u16string();
}

void HTMLElement::SetDir(const std::u16string& value) {
  SetAttributeWithoutValidation(u"dir", value);
}

bool HTMLElement::Translate() const {
  for (const Element* element = this; element;
       element = element->ParentElement()) {
    if (!element->IsHTMLElement()) {
      continue;
    }
    const std::u16string* value = element->FindAttribute(u"translate");
    if (!value) {
      continue;
    }
    if (value->empty() || base::EqualsCaseInsensitiveASCII(*value, u"yes")) {
      return true;
    }
    if (base::EqualsCaseInsensitiveASCII(*value, u"no")) {
      return false;
    }
  }
  return true;
}

void HTMLElement::SetTranslate(bool value) {
  SetAttributeWithoutValidation(u"translate", value ? u"yes" : u"no");
}

bool HTMLElement::Draggable() const {
  const std::u16string* value = FindAttribute(u"draggable");
  return value && base::EqualsCaseInsensitiveASCII(*value, u"true");
}

void HTMLElement::SetDraggable(bool value) {
  SetAttributeWithoutValidation(u"draggable", value ? u"true" : u"false");
}

bool HTMLElement::Spellcheck() const {
  // Blink's IsSpellCheckingEnabled(), with Chrome's setting on by default.
  for (const Element* element = this; element;
       element = element->ParentElement()) {
    const std::u16string* value = element->FindAttribute(u"spellcheck");
    if (!value) {
      continue;
    }
    if (value->empty() || base::EqualsCaseInsensitiveASCII(*value, u"true")) {
      return true;
    }
    if (base::EqualsCaseInsensitiveASCII(*value, u"false")) {
      return false;
    }
  }
  return true;
}

void HTMLElement::SetSpellcheck(bool value) {
  SetAttributeWithoutValidation(u"spellcheck", value ? u"true" : u"false");
}

std::u16string HTMLElement::ContentEditable() const {
  return std::u16string(ContentEditableState(*this));
}

void HTMLElement::SetContentEditable(
    const std::u16string& value,
    bindings::ExceptionState& exception_state) {
  const std::u16string lower = base::ToLowerASCII(value);
  if (lower == u"true" || lower == u"false" || lower == u"plaintext-only") {
    SetAttributeWithoutValidation(u"contenteditable", lower);
  } else if (lower == u"inherit") {
    RemoveAttribute(QualifiedName{.local_name = u"contenteditable"});
  } else {
    exception_state.ThrowDOMException(
        "SyntaxError",
        base::StrCat({"The value provided ('", base::UTF16ToUTF8(value),
                      "') is not one of 'true', 'false', 'plaintext-only', "
                      "or 'inherit'."}));
  }
}

bool HTMLElement::IsContentEditableForBinding() const {
  for (const Element* element = this; element;
       element = element->ParentElement()) {
    if (!element->IsHTMLElement()) {
      continue;
    }
    const std::u16string_view state = ContentEditableState(*element);
    if (state == u"false") {
      return false;
    }
    if (state != u"inherit") {
      return true;
    }
  }
  return IsConnected() && GetDocument().DesignMode() == u"on";
}

}  // namespace mitto
