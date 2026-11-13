#include "mitto/dom/named_node_map.h"

#include <algorithm>
#include <utility>

#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_named_node_map.h"
#include "mitto/dom/attr.h"
#include "mitto/dom/document.h"
#include "mitto/dom/element.h"

namespace mitto {

namespace {

bool HasAsciiUpper(const std::u16string& text) {
  return std::any_of(text.begin(), text.end(),
                     [](char16_t c) { return c >= 'A' && c <= 'Z'; });
}

}  // namespace

// static
NamedNodeMap* NamedNodeMap::Create(Element& element) {
  return bindings::MakeGarbageCollected<NamedNodeMap>(element);
}

NamedNodeMap::NamedNodeMap(Element& element) : element_(&element) {}

const bindings::WrapperTypeInfo* NamedNodeMap::GetWrapperTypeInfo() const {
  return &bindings::V8NamedNodeMap::kWrapperTypeInfo;
}

void NamedNodeMap::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(element_);
}

uint32_t NamedNodeMap::Length() const {
  return static_cast<uint32_t>(element_->attributes().size());
}

Attr* NamedNodeMap::Item(uint32_t index) const {
  if (index >= element_->attributes().size()) {
    return nullptr;
  }
  return element_->EnsureAttr(element_->attributes()[index].name);
}

Attr* NamedNodeMap::GetNamedItem(const std::u16string& name) const {
  return element_->GetAttributeNode(name);
}

Attr* NamedNodeMap::GetNamedItemNS(
    const std::optional<std::u16string>& namespace_uri,
    const std::u16string& local_name) const {
  return element_->GetAttributeNodeNS(namespace_uri, local_name);
}

Attr* NamedNodeMap::SetNamedItem(Attr* attr,
                                 bindings::ExceptionState& exception_state) {
  return element_->SetAttributeNode(attr, exception_state);
}

Attr* NamedNodeMap::SetNamedItemNS(Attr* attr,
                                   bindings::ExceptionState& exception_state) {
  return element_->SetAttributeNodeNS(attr, exception_state);
}

Attr* NamedNodeMap::RemoveNamedItem(const std::u16string& name,
                                    bindings::ExceptionState& exception_state) {
  Attr* const attr = element_->DetachAttributeByQualifiedName(name);
  if (!attr) {
    exception_state.ThrowDOMException(
        "NotFoundError",
        base::StrCat(
            {"No item with name '", base::UTF16ToUTF8(name), "' was found."}));
  }
  return attr;
}

Attr* NamedNodeMap::RemoveNamedItemNS(
    const std::optional<std::u16string>& namespace_uri,
    const std::u16string& local_name,
    bindings::ExceptionState& exception_state) {
  Attr* const attr = element_->DetachAttribute(
      QualifiedName{.local_name = local_name,
                    .namespace_uri = namespace_uri && !namespace_uri->empty()
                                         ? namespace_uri
                                         : std::nullopt});
  if (!attr) {
    exception_state.ThrowDOMException(
        "NotFoundError",
        base::StrCat({"No item with name '",
                      base::UTF16ToUTF8(namespace_uri.value_or(u"")),
                      "::", base::UTF16ToUTF8(local_name), "' was found."}));
  }
  return attr;
}

std::vector<std::u16string> NamedNodeMap::NamedPropertyEnumerator() const {
  const bool lower_case_only =
      element_->IsHTMLElement() && element_->GetDocument().IsHTMLDocument();
  std::vector<std::u16string> names;
  for (const Element::Attribute& attribute : element_->attributes()) {
    if (lower_case_only &&
        (HasAsciiUpper(attribute.name.prefix.value_or(u"")) ||
         HasAsciiUpper(attribute.name.local_name))) {
      continue;
    }
    std::u16string name = attribute.name.ToString();
    if (std::find(names.begin(), names.end(), name) == names.end()) {
      names.push_back(std::move(name));
    }
  }
  return names;
}

bool NamedNodeMap::NamedPropertyQuery(const std::u16string& name) const {
  const std::vector<std::u16string> names = NamedPropertyEnumerator();
  return std::find(names.begin(), names.end(), name) != names.end();
}

}  // namespace mitto
