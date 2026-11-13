// The DOM's Element, with its attributes, the selectors API and the markup
// of innerHTML and outerHTML.
//
// An element and each of its attributes has a name with a namespace, as Blink's
// QualifiedName: the HTML parser and setAttribute() make attributes of no
// namespace, and foreign content and the methods that name a namespace make
// others.
//
// Upstream: third_party/blink/renderer/core/dom/element.h
// Upstream: third_party/blink/renderer/core/dom/element.cc

#ifndef MITTO_DOM_ELEMENT_H_
#define MITTO_DOM_ELEMENT_H_

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "mitto/dom/heap_vector.h"
#include "mitto/dom/node.h"
#include "mitto/dom/qualified_name.h"

namespace mitto {

class Attr;
class DOMTokenList;
class NamedNodeMap;

class Element : public Node {
 public:
  struct Attribute {
    QualifiedName name;
    std::u16string value;
  };

  // An element of `namespace_uri`, null for none, as Document's factory makes
  // it for a name that has no class of its own.
  static Element* Create(Document& document,
                         const std::optional<std::u16string>& namespace_uri,
                         const std::optional<std::u16string>& prefix,
                         const std::u16string& local_name);

  Element(Document& document,
          const std::optional<std::u16string>& namespace_uri,
          const std::optional<std::u16string>& prefix,
          const std::u16string& local_name);
  ~Element() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  const std::optional<std::u16string>& NamespaceURI() const {
    return tag_name_.namespace_uri;
  }
  const std::optional<std::u16string>& Prefix() const {
    return tag_name_.prefix;
  }
  const std::u16string& LocalName() const { return tag_name_.local_name; }
  std::u16string TagName() const;
  std::optional<std::u16string> GetAttribute(const std::u16string& name) const;
  std::optional<std::u16string> GetAttributeNS(
      const std::optional<std::u16string>& namespace_uri,
      const std::u16string& local_name) const;
  std::vector<std::u16string> GetAttributeNamesForBindings() const;
  void SetAttribute(const std::u16string& name,
                    const std::u16string& value,
                    bindings::ExceptionState& exception_state);
  void SetAttributeNS(const std::optional<std::u16string>& namespace_uri,
                      const std::u16string& qualified_name,
                      const std::u16string& value,
                      bindings::ExceptionState& exception_state);
  void RemoveAttribute(const std::u16string& name);
  void RemoveAttributeNS(const std::optional<std::u16string>& namespace_uri,
                         const std::u16string& local_name);
  bool ToggleAttribute(const std::u16string& name,
                       std::optional<bool> force,
                       bindings::ExceptionState& exception_state);
  bool HasAttribute(const std::u16string& name) const;
  bool HasAttributeNS(const std::optional<std::u16string>& namespace_uri,
                      const std::u16string& local_name) const;
  // The same NamedNodeMap and DOMTokenLists on every read.
  NamedNodeMap* AttributesForBindings();
  DOMTokenList* ClassList();
  DOMTokenList* Part();
  Attr* GetAttributeNode(const std::u16string& name);
  Attr* GetAttributeNodeNS(const std::optional<std::u16string>& namespace_uri,
                           const std::u16string& local_name);
  Attr* SetAttributeNode(Attr* attr, bindings::ExceptionState& exception_state);
  Attr* SetAttributeNodeNS(Attr* attr,
                           bindings::ExceptionState& exception_state);
  Attr* RemoveAttributeNode(Attr* attr,
                            bindings::ExceptionState& exception_state);
  bool HasAttributes() const { return !attributes_.empty(); }
  std::u16string InnerHTML() const;
  void SetInnerHTML(const std::u16string& html,
                    bindings::ExceptionState& exception_state);
  std::u16string OuterHTML() const;
  void SetOuterHTML(const std::u16string& html,
                    bindings::ExceptionState& exception_state);
  Element* Closest(const std::u16string& selectors,
                   bindings::ExceptionState& exception_state);
  bool Matches(const std::u16string& selectors,
               bindings::ExceptionState& exception_state);
  Element* InsertAdjacentElement(const std::u16string& where,
                                 Element* element,
                                 bindings::ExceptionState& exception_state);
  void InsertAdjacentText(const std::u16string& where,
                          const std::u16string& data,
                          bindings::ExceptionState& exception_state);
  void InsertAdjacentHTML(const std::u16string& position,
                          const std::u16string& text,
                          bindings::ExceptionState& exception_state);
  // HTMLOrForeignElement's tabIndex: the attribute's integer, or else the
  // element's default, as Blink's Element::tabIndex() reads it.
  int32_t TabIndex() const;
  void SetTabIndex(int32_t value);
  // Blink's DefaultTabIndex(): 0 for an element that the HTML Standard
  // makes focusable, such as a link or a form control, and -1 otherwise.
  virtual int32_t DefaultTabIndex() const;

  // What the bindings call for a reflected attribute: the content attribute
  // `name`, which is lower case.
  std::u16string ReflectedStringAttribute(std::u16string_view name) const;
  void SetReflectedStringAttribute(std::u16string_view name,
                                   const std::u16string& value);
  bool ReflectedBooleanAttribute(std::u16string_view name) const;
  void SetReflectedBooleanAttribute(std::u16string_view name, bool value);
  // One that [ReflectOnly] limits to `keywords`, as bind_gen's
  // make_reflect_process_keyword_state() reads it: the content attribute in
  // lower case if it is one of them, and otherwise the default for a missing,
  // an empty or another value, or null, for a nullable attribute, or "".
  std::optional<std::u16string> ReflectedKeywordAttribute(
      std::u16string_view name,
      base::span<const std::u16string_view> keywords,
      std::optional<std::u16string_view> missing_default,
      std::optional<std::u16string_view> invalid_default,
      std::optional<std::u16string_view> empty_default,
      bool nullable) const;
  // A nullable one, which gives null when the content attribute is missing
  // and which null removes.
  std::optional<std::u16string> ReflectedNullableStringAttribute(
      std::u16string_view name) const;
  void SetReflectedNullableStringAttribute(
      std::u16string_view name,
      const std::optional<std::u16string>& value);
  // One of a URL, Blink's GetURLAttribute(): the content attribute without
  // its leading and trailing HTML spaces, resolved against the document's
  // base URL, or as it is if it does not resolve, and "" when it is missing.
  std::u16string ReflectedURLAttribute(std::u16string_view name) const;
  // One of a long, Blink's GetIntegralAttribute() and SetIntegralAttribute():
  // the HTML Standard's rules for parsing integers, and 0 for a content
  // attribute that is missing or does not parse.
  int32_t ReflectedIntegerAttribute(std::u16string_view name) const;
  void SetReflectedIntegerAttribute(std::u16string_view name, int32_t value);
  // One of an unsigned long, Blink's GetUnsignedIntegralAttribute() and
  // SetUnsignedIntegralAttribute(): a negative value reads as 0, and a value
  // above 2147483647 is set as 0.
  uint32_t ReflectedUnsignedIntegerAttribute(std::u16string_view name) const;
  void SetReflectedUnsignedIntegerAttribute(std::u16string_view name,
                                            uint32_t value);

  // Node's.
  uint16_t GetNodeType() const override;
  std::u16string NodeName() const override;

  // What the DOM and the parser use.
  const QualifiedName& TagQName() const { return tag_name_; }
  // Gives the element of a name with a prefix, which createElementNS() makes
  // through the factory of its namespace, that prefix, as Blink's
  // SetTagNameForCreateElementNS() does.
  void SetPrefixForCreateElementNS(const std::optional<std::u16string>& prefix);
  const std::vector<Attribute>& attributes() const { return attributes_; }
  // The value of the attribute of no namespace whose local name is `name`, or
  // null, as Blink's FastGetAttribute() reads an HTML attribute.
  const std::u16string* FindAttribute(std::u16string_view name) const;
  // The value of the attribute of `name`'s local name and namespace, or null.
  const std::u16string* FindAttribute(const QualifiedName& name) const;
  // Sets the attribute of no namespace whose local name is `name`, which the
  // caller has checked, without lower-casing it, as the parser does.
  void SetAttributeWithoutValidation(std::u16string_view name,
                                     const std::u16string& value);
  // Sets the attribute of `name`'s local name and namespace, keeping the
  // prefix of one that is there, as Blink's setAttribute(QualifiedName) does
  // and the parser does for the attributes of foreign content.
  void SetAttributeWithoutValidation(const QualifiedName& name,
                                     const std::u16string& value);
  // Removes the attribute of `name`'s local name and namespace, if any.
  void RemoveAttribute(const QualifiedName& name);
  // The node of the attribute of `name`'s local name and namespace, the same
  // one for as long as the attribute is there, as Blink's EnsureAttr().
  Attr* EnsureAttr(const QualifiedName& name);
  // Removes the attribute that `name` or the qualified name `name` finds, and
  // gives its node, which keeps its value, or null if there is none, as
  // Blink's DetachAttribute() does for removeNamedItemNS() and
  // removeNamedItem().
  Attr* DetachAttribute(const QualifiedName& name);
  Attr* DetachAttributeByQualifiedName(const std::u16string& name);
  // The DOM standard's "locate a namespace prefix" and "locate a namespace",
  // as Blink's Element::LocateNamespacePrefix() and Node::lookupNamespaceURI()
  // run them on an element.
  std::optional<std::u16string> LocateNamespacePrefix(
      const std::u16string& namespace_uri) const;
  std::optional<std::u16string> LocateNamespace(
      const std::optional<std::u16string>& prefix) const;
  // Whether it is an element of the HTML namespace.
  bool IsHTMLElement() const;
  // Whether it is an HTML element whose local name is `name`.
  bool HasHTMLTagName(std::u16string_view name) const {
    return IsHTMLElement() && tag_name_.local_name == name;
  }
  // Called once the attribute `name` has changed from `old_value` to
  // `new_value`, null for none, through the DOM and the parser alike, as
  // Blink's Element::AttributeChanged() is.
  virtual void AttributeChanged(const QualifiedName& name,
                                const std::u16string* old_value,
                                const std::u16string* new_value) {}

 protected:
  Node* CloneWithoutChildren(Document& document) const override;
  // The nodes of its attributes move with it, as Blink's
  // Element::DidMoveToNewDocument() adopts them.
  void DidMoveToNewDocument(Document& old_document) override;

 private:
  // The DOM standard: an HTML element of an HTML document matches attribute
  // names in lower case.
  std::u16string AttributeNameForLookup(const std::u16string& name) const;
  // The index of the first attribute whose qualified name is `name`, as
  // Blink's AttributeCollection::FindHinted() finds it: an attribute without
  // a prefix by its local name, which is compared with `lowered`, the name in
  // lower case where AttributeNameForLookup() says so, then one with a prefix
  // by "prefix:local", which is compared with `name` as it is.
  std::optional<size_t> FindAttributeIndexHinted(
      std::u16string_view name,
      std::u16string_view lowered) const;
  std::optional<size_t> FindAttributeIndex(const QualifiedName& name) const;
  // Every change to the attributes goes through these, as through Blink's
  // AppendAttributeInternal(), SetAttributeInternal() and
  // RemoveAttributeInternal().
  void AppendAttributeInternal(const QualifiedName& name,
                               const std::u16string& value);
  void SetAttributeValueInternal(size_t index, const std::u16string& value);
  // Detaches the attribute's node too, which keeps its value.
  void RemoveAttributeInternal(size_t index);
  // The node of the attribute of `name`'s local name and namespace, if any.
  Attr* AttrIfExists(const QualifiedName& name) const;
  // Blink's DetachAttribute(): the node of the attribute at `index`, made if
  // there is none, after the attribute goes.
  Attr* DetachAttributeAt(size_t index);
  // Blink's InsertAdjacent(): inserts `node` where `where` says, and returns
  // it, or null if it went nowhere.
  Node* InsertAdjacent(const std::u16string& where,
                       Node* node,
                       bindings::ExceptionState& exception_state);
  // Parses `html` as a fragment for `context` and returns its nodes, with no
  // parent. Throws for a document that is not HTML.
  NodeVector ParseFragment(const std::u16string& html,
                           Element& context,
                           bindings::ExceptionState& exception_state);

  QualifiedName tag_name_;
  std::vector<Attribute> attributes_;
  // The nodes of the attributes that a page has asked for, as Blink's
  // AttrNodeList.
  HeapVector<Attr> attr_nodes_;
  cppgc::Member<NamedNodeMap> attribute_map_;
  cppgc::Member<DOMTokenList> class_list_;
  cppgc::Member<DOMTokenList> part_;
};

}  // namespace mitto

#endif  // MITTO_DOM_ELEMENT_H_
