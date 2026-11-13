#include "mitto/dom/element.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "base/numerics/safe_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_element.h"
#include "mitto/dom/attr.h"
#include "mitto/dom/document.h"
#include "mitto/dom/document_fragment.h"
#include "mitto/dom/dom_token_list.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/html_template_element.h"
#include "mitto/dom/html_tree_builder.h"
#include "mitto/dom/markup_serializer.h"
#include "mitto/dom/named_node_map.h"
#include "mitto/dom/selector_query.h"
#include "mitto/dom/text.h"
#include "url/gurl.h"

namespace mitto {

namespace {

bool IsHTMLSpace(char16_t c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

// Blink's StripLeadingAndTrailingHtmlSpaces().
std::u16string_view StripHTMLSpaces(std::u16string_view value) {
  while (!value.empty() && IsHTMLSpace(value.front())) {
    value.remove_prefix(1);
  }
  while (!value.empty() && IsHTMLSpace(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

// Blink's ParseHTMLInteger(): the HTML Standard's rules for parsing integers,
// with what follows the digits ignored, and nothing for no digits or a value
// beyond an int.
std::optional<int32_t> ParseHTMLInteger(std::u16string_view value) {
  while (!value.empty() && IsHTMLSpace(value.front())) {
    value.remove_prefix(1);
  }
  bool negative = false;
  if (!value.empty() && (value.front() == '-' || value.front() == '+')) {
    negative = value.front() == '-';
    value.remove_prefix(1);
  }
  if (value.empty() || value.front() < '0' || value.front() > '9') {
    return std::nullopt;
  }
  int64_t result = 0;
  for (const char16_t c : value) {
    if (c < '0' || c > '9') {
      break;
    }
    result = result * 10 + (c - '0');
    if (result > int64_t{std::numeric_limits<int32_t>::max()} + 1) {
      return std::nullopt;
    }
  }
  if (negative) {
    result = -result;
  }
  if (!base::IsValueInRangeForNumericType<int32_t>(result)) {
    return std::nullopt;
  }
  return static_cast<int32_t>(result);
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
      tag_name_{.prefix = prefix,
                .local_name = local_name,
                .namespace_uri = namespace_uri} {}

Element::~Element() = default;

const bindings::WrapperTypeInfo* Element::GetWrapperTypeInfo() const {
  return &bindings::V8Element::kWrapperTypeInfo;
}

void Element::Trace(cppgc::Visitor* visitor) const {
  Node::Trace(visitor);
  attr_nodes_.Trace(visitor);
  visitor->Trace(attribute_map_);
  visitor->Trace(class_list_);
  visitor->Trace(part_);
}

bool Element::IsHTMLElement() const {
  return tag_name_.namespace_uri == html_names::kXhtmlNamespace;
}

void Element::SetPrefixForCreateElementNS(
    const std::optional<std::u16string>& prefix) {
  tag_name_.prefix = prefix;
}

std::u16string Element::TagName() const {
  std::u16string qualified_name = tag_name_.ToString();
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

std::optional<size_t> Element::FindAttributeIndexHinted(
    std::u16string_view name,
    std::u16string_view lowered) const {
  for (size_t i = 0; i < attributes_.size(); ++i) {
    const QualifiedName& attribute_name = attributes_[i].name;
    if (!attribute_name.prefix && attribute_name.local_name == lowered) {
      return i;
    }
  }
  for (size_t i = 0; i < attributes_.size(); ++i) {
    const QualifiedName& attribute_name = attributes_[i].name;
    if (attribute_name.prefix && attribute_name.ToString() == name) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<size_t> Element::FindAttributeIndex(
    const QualifiedName& name) const {
  for (size_t i = 0; i < attributes_.size(); ++i) {
    if (attributes_[i].name.Matches(name)) {
      return i;
    }
  }
  return std::nullopt;
}

void Element::AppendAttributeInternal(const QualifiedName& name,
                                      const std::u16string& value) {
  attributes_.push_back({name, value});
  // The hook reads the attribute as it is stored.
  const Attribute& added = attributes_.back();
  AttributeChanged(added.name, nullptr, &added.value);
}

void Element::SetAttributeValueInternal(size_t index,
                                        const std::u16string& value) {
  const std::u16string old_value =
      std::exchange(attributes_[index].value, value);
  AttributeChanged(attributes_[index].name, &old_value,
                   &attributes_[index].value);
}

void Element::RemoveAttributeInternal(size_t index) {
  // Blink's RemoveAttributeInternal(): the attribute's node keeps the value.
  if (Attr* const attr = AttrIfExists(attributes_[index].name)) {
    attr->DetachFromElementWithValue(attributes_[index].value);
    attr_nodes_.EraseIf([attr](Attr* node) { return node == attr; });
  }
  const Attribute removed = std::move(attributes_[index]);
  attributes_.erase(attributes_.begin() + index);
  AttributeChanged(removed.name, &removed.value, nullptr);
}

Attr* Element::AttrIfExists(const QualifiedName& name) const {
  for (size_t i = 0; i < attr_nodes_.size(); ++i) {
    if (attr_nodes_[i]->GetQualifiedName().Matches(name)) {
      return attr_nodes_[i];
    }
  }
  return nullptr;
}

Attr* Element::EnsureAttr(const QualifiedName& name) {
  if (Attr* const attr = AttrIfExists(name)) {
    return attr;
  }
  Attr* const attr = Attr::Create(*this, name);
  attr_nodes_.push_back(attr);
  return attr;
}

Attr* Element::DetachAttributeAt(size_t index) {
  Attr* attr = AttrIfExists(attributes_[index].name);
  if (!attr) {
    attr = Attr::Create(GetDocument(), attributes_[index].name,
                        attributes_[index].value);
  }
  RemoveAttributeInternal(index);
  return attr;
}

Attr* Element::DetachAttribute(const QualifiedName& name) {
  const std::optional<size_t> index = FindAttributeIndex(name);
  return index ? DetachAttributeAt(*index) : nullptr;
}

Attr* Element::DetachAttributeByQualifiedName(const std::u16string& name) {
  const std::optional<size_t> index =
      FindAttributeIndexHinted(name, AttributeNameForLookup(name));
  return index ? DetachAttributeAt(*index) : nullptr;
}

NamedNodeMap* Element::AttributesForBindings() {
  if (!attribute_map_) {
    attribute_map_ = NamedNodeMap::Create(*this);
  }
  return attribute_map_.Get();
}

DOMTokenList* Element::ClassList() {
  if (!class_list_) {
    class_list_ = DOMTokenList::Create(*this, u"class");
  }
  return class_list_.Get();
}

DOMTokenList* Element::Part() {
  if (!part_) {
    part_ = DOMTokenList::Create(*this, u"part");
  }
  return part_.Get();
}

Attr* Element::GetAttributeNode(const std::u16string& name) {
  const std::optional<size_t> index =
      FindAttributeIndexHinted(name, AttributeNameForLookup(name));
  return index ? EnsureAttr(attributes_[*index].name) : nullptr;
}

Attr* Element::GetAttributeNodeNS(
    const std::optional<std::u16string>& namespace_uri,
    const std::u16string& local_name) {
  const std::optional<size_t> index = FindAttributeIndex(
      QualifiedName{.local_name = local_name,
                    .namespace_uri = namespace_uri && !namespace_uri->empty()
                                         ? namespace_uri
                                         : std::nullopt});
  return index ? EnsureAttr(attributes_[*index].name) : nullptr;
}

Attr* Element::SetAttributeNode(Attr* attr,
                                bindings::ExceptionState& exception_state) {
  // Blink's Element::setAttributeNode().
  const QualifiedName& name = attr->GetQualifiedName();
  Attr* old_attr = AttrIfExists(name);
  if (old_attr == attr) {
    return attr;
  }
  if (attr->OwnerElement()) {
    exception_state.ThrowDOMException(
        "InUseAttributeError",
        "The node provided is an attribute node that is already an attribute "
        "of another Element; attribute nodes must be explicitly cloned.");
    return nullptr;
  }
  const std::u16string value = attr->Value();
  const std::optional<size_t> index = FindAttributeIndex(name);
  if (index) {
    if (old_attr) {
      old_attr->DetachFromElementWithValue(attributes_[*index].value);
      attr_nodes_.EraseIf([old_attr](Attr* node) { return node == old_attr; });
    } else {
      old_attr = Attr::Create(GetDocument(), name, attributes_[*index].value);
    }
    SetAttributeValueInternal(*index, value);
  } else {
    AppendAttributeInternal(name, value);
  }
  attr->AttachToElement(*this);
  attr->SetTreeDocument(GetDocument());
  attr_nodes_.push_back(attr);
  return old_attr;
}

Attr* Element::SetAttributeNodeNS(Attr* attr,
                                  bindings::ExceptionState& exception_state) {
  return SetAttributeNode(attr, exception_state);
}

Attr* Element::RemoveAttributeNode(Attr* attr,
                                   bindings::ExceptionState& exception_state) {
  if (attr->OwnerElement() != this) {
    exception_state.ThrowDOMException(
        "NotFoundError", "The node provided is owned by another element.");
    return nullptr;
  }
  const std::optional<size_t> index =
      FindAttributeIndex(attr->GetQualifiedName());
  if (!index) {
    exception_state.ThrowDOMException(
        "NotFoundError", "The attribute was not found on this element.");
    return nullptr;
  }
  RemoveAttributeInternal(*index);
  return attr;
}

const std::u16string* Element::FindAttribute(std::u16string_view name) const {
  for (const Attribute& attribute : attributes_) {
    if (!attribute.name.namespace_uri && attribute.name.local_name == name) {
      return &attribute.value;
    }
  }
  return nullptr;
}

const std::u16string* Element::FindAttribute(const QualifiedName& name) const {
  const std::optional<size_t> index = FindAttributeIndex(name);
  return index ? &attributes_[*index].value : nullptr;
}

std::optional<std::u16string> Element::GetAttribute(
    const std::u16string& name) const {
  if (const std::optional<size_t> index =
          FindAttributeIndexHinted(name, AttributeNameForLookup(name))) {
    return attributes_[*index].value;
  }
  return std::nullopt;
}

std::optional<std::u16string> Element::GetAttributeNS(
    const std::optional<std::u16string>& namespace_uri,
    const std::u16string& local_name) const {
  // A QualifiedName of an empty namespace has none, as Blink's.
  const std::u16string* value = FindAttribute(
      QualifiedName{.local_name = local_name,
                    .namespace_uri = namespace_uri && !namespace_uri->empty()
                                         ? namespace_uri
                                         : std::nullopt});
  if (value) {
    return *value;
  }
  return std::nullopt;
}

std::vector<std::u16string> Element::GetAttributeNamesForBindings() const {
  std::vector<std::u16string> names;
  names.reserve(attributes_.size());
  for (const Attribute& attribute : attributes_) {
    names.push_back(attribute.name.ToString());
  }
  return names;
}

void Element::SetAttributeWithoutValidation(std::u16string_view name,
                                            const std::u16string& value) {
  SetAttributeWithoutValidation(
      QualifiedName{.local_name = std::u16string(name)}, value);
}

void Element::SetAttributeWithoutValidation(const QualifiedName& name,
                                            const std::u16string& value) {
  if (const std::optional<size_t> index = FindAttributeIndex(name)) {
    SetAttributeValueInternal(*index, value);
    return;
  }
  AppendAttributeInternal(name, value);
}

void Element::RemoveAttribute(const QualifiedName& name) {
  if (const std::optional<size_t> index = FindAttributeIndex(name)) {
    RemoveAttributeInternal(*index);
  }
}

void Element::SetAttribute(const std::u16string& name,
                           const std::u16string& value,
                           bindings::ExceptionState& exception_state) {
  if (!IsValidAttributeLocalName(name)) {
    ThrowInvalidAttributeName(name, exception_state);
    return;
  }
  // Blink's SetAttributeHinted(): the attribute that the qualified name
  // finds, or a new one of no namespace.
  const std::u16string lookup = AttributeNameForLookup(name);
  if (const std::optional<size_t> index =
          FindAttributeIndexHinted(name, lookup)) {
    SetAttributeValueInternal(*index, value);
    return;
  }
  AppendAttributeInternal(QualifiedName{.local_name = lookup}, value);
}

void Element::SetAttributeNS(const std::optional<std::u16string>& namespace_uri,
                             const std::u16string& qualified_name,
                             const std::u16string& value,
                             bindings::ExceptionState& exception_state) {
  // Blink's ParseAttributeName().
  const std::optional<QualifiedName> name = QualifiedName::Parse(
      namespace_uri, qualified_name, QualifiedName::ParsingMode::kAttribute,
      exception_state);
  if (!name) {
    return;
  }
  if (!name->HasValidNamespace()) {
    exception_state.ThrowDOMException(
        "NamespaceError",
        base::StrCat({"'", base::UTF16ToUTF8(namespace_uri.value_or(u"")),
                      "' is an invalid namespace for attributes."}));
    return;
  }
  SetAttributeWithoutValidation(*name, value);
}

void Element::RemoveAttribute(const std::u16string& name) {
  if (const std::optional<size_t> index =
          FindAttributeIndexHinted(name, AttributeNameForLookup(name))) {
    RemoveAttributeInternal(*index);
  }
}

void Element::RemoveAttributeNS(
    const std::optional<std::u16string>& namespace_uri,
    const std::u16string& local_name) {
  RemoveAttribute(
      QualifiedName{.local_name = local_name,
                    .namespace_uri = namespace_uri && !namespace_uri->empty()
                                         ? namespace_uri
                                         : std::nullopt});
}

bool Element::ToggleAttribute(const std::u16string& name,
                              std::optional<bool> force,
                              bindings::ExceptionState& exception_state) {
  if (!IsValidAttributeLocalName(name)) {
    ThrowInvalidAttributeName(name, exception_state);
    return false;
  }
  // Blink's toggleAttribute() looks up the lower-case name both ways.
  const std::u16string lookup = AttributeNameForLookup(name);
  const std::optional<size_t> index = FindAttributeIndexHinted(lookup, lookup);
  if (!index) {
    if (force.value_or(true)) {
      AppendAttributeInternal(QualifiedName{.local_name = lookup},
                              std::u16string());
      return true;
    }
    return false;
  }
  if (!force.value_or(false)) {
    RemoveAttributeInternal(*index);
    return false;
  }
  return true;
}

bool Element::HasAttribute(const std::u16string& name) const {
  return FindAttributeIndexHinted(name, AttributeNameForLookup(name))
      .has_value();
}

bool Element::HasAttributeNS(const std::optional<std::u16string>& namespace_uri,
                             const std::u16string& local_name) const {
  return GetAttributeNS(namespace_uri, local_name).has_value();
}

std::optional<std::u16string> Element::LocateNamespacePrefix(
    const std::u16string& namespace_uri) const {
  for (const Element* element = this; element;
       element = element->ParentElement()) {
    if (element->Prefix() && element->NamespaceURI() == namespace_uri) {
      return element->Prefix();
    }
    for (const Attribute& attribute : element->attributes_) {
      if (attribute.name.prefix == u"xmlns" &&
          attribute.value == namespace_uri) {
        return attribute.name.local_name;
      }
    }
  }
  return std::nullopt;
}

std::optional<std::u16string> Element::LocateNamespace(
    const std::optional<std::u16string>& prefix) const {
  if (prefix == u"xml") {
    return std::u16string(html_names::kXmlNamespace);
  }
  if (prefix == u"xmlns") {
    return std::u16string(html_names::kXmlnsNamespace);
  }
  for (const Element* element = this; element;
       element = element->ParentElement()) {
    if (element->NamespaceURI() && element->Prefix() == prefix) {
      return element->NamespaceURI();
    }
    // As Blink, which looks at an attribute's prefix and local name, not its
    // namespace.
    for (const Attribute& attribute : element->attributes_) {
      if ((attribute.name.prefix == u"xmlns" &&
           attribute.name.local_name == prefix) ||
          (attribute.name.local_name == u"xmlns" && !prefix)) {
        if (attribute.value.empty()) {
          return std::nullopt;
        }
        return attribute.value;
      }
    }
  }
  return std::nullopt;
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

// Upstream: third_party/blink/renderer/bindings/scripts/bind_gen/interface.py
std::optional<std::u16string> Element::ReflectedKeywordAttribute(
    std::u16string_view name,
    base::span<const std::u16string_view> keywords,
    std::optional<std::u16string_view> missing_default,
    std::optional<std::u16string_view> invalid_default,
    std::optional<std::u16string_view> empty_default,
    bool nullable) const {
  // A missing attribute is Blink's null string, which is empty but equals no
  // keyword, not even "".
  const std::u16string* const value = FindAttribute(name);
  if (!value && missing_default) {
    return std::u16string(*missing_default);
  }
  if (!value && nullable && !missing_default) {
    return std::nullopt;
  }
  if (empty_default && (!value || value->empty())) {
    return std::u16string(*empty_default);
  }
  if (value) {
    const std::u16string lower = base::ToLowerASCII(*value);
    if (std::find(keywords.begin(), keywords.end(), lower) != keywords.end()) {
      return lower;
    }
  }
  if (invalid_default) {
    return std::u16string(*invalid_default);
  }
  if (nullable) {
    return std::nullopt;
  }
  return std::u16string();
}

std::optional<std::u16string> Element::ReflectedNullableStringAttribute(
    std::u16string_view name) const {
  if (const std::u16string* value = FindAttribute(name)) {
    return *value;
  }
  return std::nullopt;
}

std::u16string Element::ReflectedURLAttribute(std::u16string_view name) const {
  const std::u16string* value = FindAttribute(name);
  if (!value) {
    return std::u16string();
  }
  const std::u16string_view stripped = StripHTMLSpaces(*value);
  const GURL url = GetDocument().CompleteUrl(stripped);
  return url.is_valid() ? base::UTF8ToUTF16(url.spec())
                        : std::u16string(stripped);
}

int32_t Element::ReflectedIntegerAttribute(std::u16string_view name) const {
  const std::u16string* value = FindAttribute(name);
  return value ? ParseHTMLInteger(*value).value_or(0) : 0;
}

void Element::SetReflectedIntegerAttribute(std::u16string_view name,
                                           int32_t value) {
  SetAttributeWithoutValidation(name, base::NumberToString16(value));
}

uint32_t Element::ReflectedUnsignedIntegerAttribute(
    std::u16string_view name) const {
  return static_cast<uint32_t>(std::max(0, ReflectedIntegerAttribute(name)));
}

void Element::SetReflectedUnsignedIntegerAttribute(std::u16string_view name,
                                                   uint32_t value) {
  // The HTML Standard's limit for an unsigned long that reflects a content
  // attribute, whose default here is 0.
  SetAttributeWithoutValidation(
      name, base::NumberToString16(value > 0x7fffffffu ? 0u : value));
}

void Element::SetReflectedNullableStringAttribute(
    std::u16string_view name,
    const std::optional<std::u16string>& value) {
  if (value) {
    SetAttributeWithoutValidation(name, *value);
    return;
  }
  RemoveAttribute(QualifiedName{.local_name = std::u16string(name)});
}

void Element::SetReflectedBooleanAttribute(std::u16string_view name,
                                           bool value) {
  if (value) {
    SetAttributeWithoutValidation(name, std::u16string());
    return;
  }
  RemoveAttribute(QualifiedName{.local_name = std::u16string(name)});
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
  // A template's markup goes into its contents.
  Node& container =
      HasHTMLTagName(u"template")
          ? static_cast<Node&>(
                *static_cast<HTMLTemplateElement*>(this)->Content())
          : static_cast<Node&>(*this);
  container.RemoveChildren();
  for (const cppgc::Persistent<Node>& node : nodes) {
    container.ParserAppendChild(node.Get());
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
  // A fragment's child parses in a new body element, as Blink's
  // setOuterHTML() and the HTML Standard have it.
  const cppgc::Persistent<Element> context(
      parent->IsElementNode()
          ? static_cast<Element*>(parent)
          : GetDocument().CreateElement(html_names::kXhtmlNamespace,
                                        std::nullopt, u"body"));
  const NodeVector nodes = ParseFragment(html, *context, exception_state);
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
  Element* const copy = document.CreateElement(
      tag_name_.namespace_uri, tag_name_.prefix, tag_name_.local_name);
  copy->attributes_ = attributes_;
  return copy;
}

void Element::DidMoveToNewDocument(Document& old_document) {
  for (Attr* const attr : attr_nodes_.ToVector()) {
    attr->SetTreeDocument(GetDocument());
  }
}

uint16_t Element::GetNodeType() const {
  return kElementNode;
}

std::u16string Element::NodeName() const {
  return TagName();
}

}  // namespace mitto
