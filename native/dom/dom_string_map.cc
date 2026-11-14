#include "mitto/dom/dom_string_map.h"

#include <string_view>

#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_dom_string_map.h"
#include "mitto/dom/element.h"

namespace mitto {

namespace {

constexpr std::u16string_view kPrefix = u"data-";

bool IsAsciiUpper(char16_t c) {
  return c >= 'A' && c <= 'Z';
}

bool IsAsciiLower(char16_t c) {
  return c >= 'a' && c <= 'z';
}

// Blink's IsValidAttributeName(): data- and no upper-case ASCII after it.
bool IsValidAttributeName(std::u16string_view name) {
  if (!name.starts_with(kPrefix)) {
    return false;
  }
  for (const char16_t c : name.substr(kPrefix.size())) {
    if (IsAsciiUpper(c)) {
      return false;
    }
  }
  return true;
}

// Blink's ConvertAttributeNameToPropertyName(): a '-' before a lower-case
// letter makes that letter upper case.
std::u16string ConvertAttributeNameToPropertyName(std::u16string_view name) {
  std::u16string result;
  for (size_t i = kPrefix.size(); i < name.size(); ++i) {
    if (name[i] == '-' && i + 1 < name.size() && IsAsciiLower(name[i + 1])) {
      result += static_cast<char16_t>(name[i + 1] - 'a' + 'A');
      ++i;
    } else {
      result += name[i];
    }
  }
  return result;
}

// Blink's PropertyNameMatchesAttributeName().
bool PropertyNameMatchesAttributeName(std::u16string_view property,
                                      std::u16string_view attribute) {
  size_t a = kPrefix.size();
  size_t p = 0;
  bool word_boundary = false;
  while (a < attribute.size() && p < property.size()) {
    const char16_t c = attribute[a];
    if (c == '-' && a + 1 < attribute.size() &&
        IsAsciiLower(attribute[a + 1])) {
      word_boundary = true;
    } else {
      const char16_t expected =
          word_boundary && IsAsciiLower(c) ? c - 'a' + 'A' : c;
      if (expected != property[p]) {
        return false;
      }
      ++p;
      word_boundary = false;
    }
    ++a;
  }
  return a == attribute.size() && p == property.size();
}

// Blink's IsValidPropertyName(): no '-' before a lower-case letter.
bool IsValidPropertyName(std::u16string_view name) {
  for (size_t i = 0; i + 1 < name.size(); ++i) {
    if (name[i] == '-' && IsAsciiLower(name[i + 1])) {
      return false;
    }
  }
  return true;
}

// Blink's ConvertPropertyNameToAttributeName().
std::u16string ConvertPropertyNameToAttributeName(std::u16string_view name) {
  std::u16string result(kPrefix);
  for (const char16_t c : name) {
    if (IsAsciiUpper(c)) {
      result += '-';
      result += static_cast<char16_t>(c - 'A' + 'a');
    } else {
      result += c;
    }
  }
  return result;
}

}  // namespace

// static
DOMStringMap* DOMStringMap::Create(Element& element) {
  return bindings::MakeGarbageCollected<DOMStringMap>(element);
}

DOMStringMap::DOMStringMap(Element& element) : element_(&element) {}

const bindings::WrapperTypeInfo* DOMStringMap::GetWrapperTypeInfo() const {
  return &bindings::V8DOMStringMap::kWrapperTypeInfo;
}

void DOMStringMap::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(element_);
}

std::optional<std::u16string> DOMStringMap::AnonymousNamedGetter(
    const std::u16string& name) const {
  for (const Element::Attribute& attribute : element_->attributes()) {
    const std::u16string& local_name = attribute.name.local_name;
    if (IsValidAttributeName(local_name) &&
        PropertyNameMatchesAttributeName(name, local_name)) {
      return attribute.value;
    }
  }
  return std::nullopt;
}

void DOMStringMap::AnonymousNamedSetter(
    const std::u16string& name,
    const std::u16string& value,
    bindings::ExceptionState& exception_state) {
  if (!IsValidPropertyName(name)) {
    exception_state.ThrowDOMException(
        "SyntaxError", base::StrCat({"'", base::UTF16ToUTF8(name),
                                     "' is not a valid property name."}));
    return;
  }
  element_->SetAttribute(ConvertPropertyNameToAttributeName(name), value,
                         exception_state);
}

bool DOMStringMap::AnonymousNamedDeleter(const std::u16string& name) {
  if (!IsValidPropertyName(name)) {
    return false;
  }
  const std::u16string attribute_name =
      ConvertPropertyNameToAttributeName(name);
  if (!element_->HasAttribute(attribute_name)) {
    return false;
  }
  element_->RemoveAttribute(attribute_name);
  return true;
}

std::vector<std::u16string> DOMStringMap::NamedPropertyEnumerator() const {
  std::vector<std::u16string> names;
  for (const Element::Attribute& attribute : element_->attributes()) {
    if (IsValidAttributeName(attribute.name.local_name)) {
      names.push_back(
          ConvertAttributeNameToPropertyName(attribute.name.local_name));
    }
  }
  return names;
}

bool DOMStringMap::NamedPropertyQuery(const std::u16string& name) const {
  return AnonymousNamedGetter(name).has_value();
}

}  // namespace mitto
