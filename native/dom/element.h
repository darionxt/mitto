// The DOM's Element, with its attributes, the selectors API and the markup
// of innerHTML and outerHTML.
//
// An attribute here has a qualified name and no namespace, as the HTML parser
// and setAttribute() make them; the methods that name a namespace are Known
// Gaps until foreign content needs them (roadmap, M8).
//
// Upstream: third_party/blink/renderer/core/dom/element.h
// Upstream: third_party/blink/renderer/core/dom/element.cc

#ifndef MITTO_DOM_ELEMENT_H_
#define MITTO_DOM_ELEMENT_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mitto/dom/node.h"

namespace mitto {

class Element : public Node {
 public:
  struct Attribute {
    std::u16string name;
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

  // The members of the interface.
  const std::optional<std::u16string>& NamespaceURI() const {
    return namespace_uri_;
  }
  const std::optional<std::u16string>& Prefix() const { return prefix_; }
  const std::u16string& LocalName() const { return local_name_; }
  std::u16string TagName() const;
  std::optional<std::u16string> GetAttribute(const std::u16string& name) const;
  std::vector<std::u16string> GetAttributeNamesForBindings() const;
  void SetAttribute(const std::u16string& name,
                    const std::u16string& value,
                    bindings::ExceptionState& exception_state);
  void RemoveAttribute(const std::u16string& name);
  bool ToggleAttribute(const std::u16string& name,
                       std::optional<bool> force,
                       bindings::ExceptionState& exception_state);
  bool HasAttribute(const std::u16string& name) const;
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

  // What the bindings call for a reflected attribute: the content attribute
  // `name`, which is lower case.
  std::u16string ReflectedStringAttribute(std::u16string_view name) const;
  void SetReflectedStringAttribute(std::u16string_view name,
                                   const std::u16string& value);
  bool ReflectedBooleanAttribute(std::u16string_view name) const;
  void SetReflectedBooleanAttribute(std::u16string_view name, bool value);

  // Node's.
  uint16_t GetNodeType() const override;
  std::u16string NodeName() const override;

  // What the DOM and the parser use.
  const std::vector<Attribute>& attributes() const { return attributes_; }
  // The value of the attribute of exactly this name, or null.
  const std::u16string* FindAttribute(std::u16string_view name) const;
  // Sets an attribute whose name the caller has checked, without lower-casing
  // it, as the parser does.
  void SetAttributeWithoutValidation(std::u16string_view name,
                                     const std::u16string& value);
  // Whether it is an element of the HTML namespace.
  bool IsHTMLElement() const;
  // Whether it is an HTML element whose local name is `name`.
  bool HasHTMLTagName(std::u16string_view name) const {
    return IsHTMLElement() && local_name_ == name;
  }

 protected:
  Node* CloneWithoutChildren(Document& document) const override;

 private:
  // The DOM standard: an HTML element of an HTML document matches attribute
  // names in lower case.
  std::u16string AttributeNameForLookup(const std::u16string& name) const;
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

  const std::optional<std::u16string> namespace_uri_;
  const std::optional<std::u16string> prefix_;
  const std::u16string local_name_;
  std::vector<Attribute> attributes_;
};

}  // namespace mitto

#endif  // MITTO_DOM_ELEMENT_H_
