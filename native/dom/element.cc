#include "mitto/dom/element.h"

#include <algorithm>

#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_element.h"
#include "mitto/dom/document.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/html_tree_builder.h"
#include "mitto/dom/markup_serializer.h"
#include "mitto/dom/selector_query.h"
#include "mitto/dom/text.h"

namespace mitto {

namespace {

// Blink's IsValidAttributeLocalName(), the DOM standard's valid attribute
// local name: not empty, and none of ASCII whitespace, U+0000, '/', '=' and
// '>'.
bool IsValidAttributeLocalName(std::u16string_view name) {
  if (name.empty()) {
    return false;
  }
  for (const char16_t c : name) {
    if (c == 0 || c == ' ' || c == '\t' || c == '\n' || c == '\f' ||
        c == '\r' || c == '/' || c == '=' || c == '>') {
      return false;
    }
  }
  return true;
}

void ThrowInvalidAttributeName(const std::u16string& name,
                               bindings::ExceptionState& exception_state) {
  exception_state.ThrowDOMException(
      "InvalidCharacterError",
      base::StrCat(
          {"'", base::UTF16ToUTF8(name), "' is not a valid attribute name."}));
}

}  // namespace

// static
Element* Element::Create(Document& document,
                         const std::optional<std::u16string>& namespace_uri,
                         const std::optional<std::u16string>& prefix,
                         const std::u16string& local_name) {
  return bindings::MakeGarbageCollected<Element>(document, namespace_uri,
                                                 prefix, local_name);
}

Element::Element(Document& document,
                 const std::optional<std::u16string>& namespace_uri,
                 const std::optional<std::u16string>& prefix,
                 const std::u16string& local_name)
    : Node(&document),
      namespace_uri_(namespace_uri),
      prefix_(prefix),
      local_name_(local_name) {}

Element::~Element() = default;

const bindings::WrapperTypeInfo* Element::GetWrapperTypeInfo() const {
  return &bindings::V8Element::kWrapperTypeInfo;
}

bool Element::IsHTMLElement() const {
  return namespace_uri_ == html_names::kXhtmlNamespace;
}

std::u16string Element::TagName() const {
  std::u16string qualified_name =
      prefix_ ? base::StrCat({*prefix_, u":", local_name_}) : local_name_;
  if (IsHTMLElement() && GetDocument().IsHTMLDocument()) {
    return base::ToUpperASCII(qualified_name);
  }
  return qualified_name;
}

std::u16string Element::AttributeNameForLookup(
    const std::u16string& name) const {
  if (IsHTMLElement() && GetDocument().IsHTMLDocument()) {
    return base::ToLowerASCII(name);
  }
  return name;
}

const std::u16string* Element::FindAttribute(std::u16string_view name) const {
  for (const Attribute& attribute : attributes_) {
    if (attribute.name == name) {
      return &attribute.value;
    }
  }
  return nullptr;
}

std::optional<std::u16string> Element::GetAttribute(
    const std::u16string& name) const {
  if (const std::u16string* value =
          FindAttribute(AttributeNameForLookup(name))) {
    return *value;
  }
  return std::nullopt;
}

std::vector<std::u16string> Element::GetAttributeNamesForBindings() const {
  std::vector<std::u16string> names;
  names.reserve(attributes_.size());
  for (const Attribute& attribute : attributes_) {
    names.push_back(attribute.name);
  }
  return names;
}

void Element::SetAttributeWithoutValidation(std::u16string_view name,
                                            const std::u16string& value) {
  for (Attribute& attribute : attributes_) {
    if (attribute.name == name) {
      attribute.value = value;
      return;
    }
  }
  attributes_.push_back({std::u16string(name), value});
}

void Element::SetAttribute(const std::u16string& name,
                           const std::u16string& value,
                           bindings::ExceptionState& exception_state) {
  if (!IsValidAttributeLocalName(name)) {
    ThrowInvalidAttributeName(name, exception_state);
    return;
  }
  SetAttributeWithoutValidation(AttributeNameForLookup(name), value);
}

void Element::RemoveAttribute(const std::u16string& name) {
  const std::u16string lookup = AttributeNameForLookup(name);
  const auto found = std::find_if(attributes_.begin(), attributes_.end(),
                                  [&lookup](const Attribute& attribute) {
                                    return attribute.name == lookup;
                                  });
  if (found != attributes_.end()) {
    attributes_.erase(found);
  }
}

bool Element::ToggleAttribute(const std::u16string& name,
                              std::optional<bool> force,
                              bindings::ExceptionState& exception_state) {
  if (!IsValidAttributeLocalName(name)) {
    ThrowInvalidAttributeName(name, exception_state);
    return false;
  }
  const std::u16string lookup = AttributeNameForLookup(name);
  if (!FindAttribute(lookup)) {
    if (force.value_or(true)) {
      SetAttributeWithoutValidation(lookup, std::u16string());
      return true;
    }
    return false;
  }
  if (!force.value_or(false)) {
    RemoveAttribute(lookup);
    return false;
  }
  return true;
}

bool Element::HasAttribute(const std::u16string& name) const {
  return FindAttribute(AttributeNameForLookup(name));
}

std::u16string Element::ReflectedStringAttribute(
    std::u16string_view name) const {
  const std::u16string* value = FindAttribute(name);
  return value ? *value : std::u16string();
}

void Element::SetReflectedStringAttribute(std::u16string_view name,
                                          const std::u16string& value) {
  SetAttributeWithoutValidation(name, value);
}

bool Element::ReflectedBooleanAttribute(std::u16string_view name) const {
  return FindAttribute(name);
}

void Element::SetReflectedBooleanAttribute(std::u16string_view name,
                                           bool value) {
  if (value) {
    SetAttributeWithoutValidation(name, std::u16string());
    return;
  }
  const auto found = std::find_if(
      attributes_.begin(), attributes_.end(),
      [name](const Attribute& attribute) { return attribute.name == name; });
  if (found != attributes_.end()) {
    attributes_.erase(found);
  }
}

std::u16string Element::InnerHTML() const {
  return SerializeChildren(*this);
}

std::u16string Element::OuterHTML() const {
  return SerializeNode(*this);
}

NodeVector Element::ParseFragment(const std::u16string& html,
                                  Element& context,
                                  bindings::ExceptionState& exception_state) {
  if (!GetDocument().IsHTMLDocument()) {
    // A document that is not HTML parses markup as XML, which the Runtime
    // does not have yet.
    exception_state.ThrowTypeError("Not implemented.");
    return {};
  }
  return ParseHtmlFragment(context, html);
}

void Element::SetInnerHTML(const std::u16string& html,
                           bindings::ExceptionState& exception_state) {
  const NodeVector nodes = ParseFragment(html, *this, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  RemoveChildren();
  for (const cppgc::Persistent<Node>& node : nodes) {
    ParserAppendChild(node.Get());
  }
}

void Element::SetOuterHTML(const std::u16string& html,
                           bindings::ExceptionState& exception_state) {
  Node* const parent = ParentNode();
  if (!parent) {
    return;
  }
  if (parent->IsDocumentNode()) {
    exception_state.ThrowDOMException(
        "NoModificationAllowedError",
        "This element's parent is a Document, which may not be modified.");
    return;
  }
  const NodeVector nodes =
      ParseFragment(html, static_cast<Element&>(*parent), exception_state);
  if (exception_state.HadException()) {
    return;
  }
  const cppgc::Persistent<Element> self(this);
  Node* const next = NextSibling();
  parent->ParserRemoveChild(*this);
  for (const cppgc::Persistent<Node>& node : nodes) {
    if (next && next->ParentNode() == parent) {
      parent->ParserInsertBefore(node.Get(), *next);
    } else {
      parent->ParserAppendChild(node.Get());
    }
  }
}

Element* Element::Closest(const std::u16string& selectors,
                          bindings::ExceptionState& exception_state) {
  const std::optional<SelectorQuery> query =
      SelectorQuery::Parse(selectors, exception_state);
  if (!query) {
    return nullptr;
  }
  for (Node* node = this; node && node->IsElementNode();
       node = node->ParentNode()) {
    if (query->Matches(static_cast<Element&>(*node))) {
      return static_cast<Element*>(node);
    }
  }
  return nullptr;
}

bool Element::Matches(const std::u16string& selectors,
                      bindings::ExceptionState& exception_state) {
  const std::optional<SelectorQuery> query =
      SelectorQuery::Parse(selectors, exception_state);
  return query && query->Matches(*this);
}

Element* Element::InsertAdjacentElement(
    const std::u16string& where,
    Element* element,
    bindings::ExceptionState& exception_state) {
  return static_cast<Element*>(InsertAdjacent(where, element, exception_state));
}

Node* Element::InsertAdjacent(const std::u16string& where,
                              Node* element,
                              bindings::ExceptionState& exception_state) {
  // Blink's Element::InsertAdjacent().
  Node* const parent = ParentNode();
  if (base::EqualsCaseInsensitiveASCII(where, u"beforeBegin")) {
    if (parent) {
      parent->InsertBefore(element, this, exception_state);
      if (!exception_state.HadException()) {
        return element;
      }
    }
    return nullptr;
  }
  if (base::EqualsCaseInsensitiveASCII(where, u"afterBegin")) {
    InsertBefore(element, FirstChild(), exception_state);
    return exception_state.HadException() ? nullptr : element;
  }
  if (base::EqualsCaseInsensitiveASCII(where, u"beforeEnd")) {
    AppendChild(element, exception_state);
    return exception_state.HadException() ? nullptr : element;
  }
  if (base::EqualsCaseInsensitiveASCII(where, u"afterEnd")) {
    if (parent) {
      parent->InsertBefore(element, NextSibling(), exception_state);
      if (!exception_state.HadException()) {
        return element;
      }
    }
    return nullptr;
  }
  exception_state.ThrowDOMException(
      "SyntaxError",
      base::StrCat({"The value provided ('", base::UTF16ToUTF8(where),
                    "') is not one of 'beforeBegin', 'afterBegin', "
                    "'beforeEnd', or 'afterEnd'."}));
  return nullptr;
}

void Element::InsertAdjacentText(const std::u16string& where,
                                 const std::u16string& data,
                                 bindings::ExceptionState& exception_state) {
  InsertAdjacent(where, Text::Create(GetDocument(), data), exception_state);
}

Node* Element::CloneWithoutChildren(Document& document) const {
  Element* const copy =
      document.CreateElement(namespace_uri_, prefix_, local_name_);
  for (const Attribute& attribute : attributes_) {
    copy->SetAttributeWithoutValidation(attribute.name, attribute.value);
  }
  return copy;
}

uint16_t Element::GetNodeType() const {
  return kElementNode;
}

std::u16string Element::NodeName() const {
  return TagName();
}

}  // namespace mitto
