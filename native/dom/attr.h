// The DOM's Attr: an attribute as a node, which reads and writes the
// attribute of its element while it has one, and holds its own value once it
// has none.
//
// Upstream: third_party/blink/renderer/core/dom/attr.h
// Upstream: third_party/blink/renderer/core/dom/attr.cc

#ifndef MITTO_DOM_ATTR_H_
#define MITTO_DOM_ATTR_H_

#include <stdint.h>

#include <optional>
#include <string>

#include "mitto/dom/node.h"
#include "mitto/dom/qualified_name.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

class Attr final : public Node {
 public:
  // The node of `element`'s attribute of `name`.
  static Attr* Create(Element& element, const QualifiedName& name);
  // One with no element, as createAttribute() makes it.
  static Attr* Create(Document& document,
                      const QualifiedName& name,
                      const std::u16string& value);

  Attr(Element& element, const QualifiedName& name);
  Attr(Document& document,
       const QualifiedName& name,
       const std::u16string& value);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  const std::optional<std::u16string>& NamespaceURI() const {
    return name_.namespace_uri;
  }
  const std::optional<std::u16string>& Prefix() const { return name_.prefix; }
  const std::u16string& LocalName() const { return name_.local_name; }
  std::u16string Name() const { return name_.ToString(); }
  std::u16string Value() const;
  void SetValue(const std::u16string& value,
                bindings::ExceptionState& exception_state);
  Element* OwnerElement() const { return element_.Get(); }
  bool Specified() const { return true; }

  // Node's.
  uint16_t GetNodeType() const override;
  std::u16string NodeName() const override;
  std::optional<std::u16string> NodeValue() const override;
  void SetNodeValue(const std::optional<std::u16string>& value,
                    bindings::ExceptionState& exception_state) override;
  std::optional<std::u16string> TextContentForBinding() const override;
  void SetTextContentForBinding(
      const std::optional<std::u16string>& value,
      bindings::ExceptionState& exception_state) override;

  // What the element uses. The name that the element's attribute is found
  // by, Blink's GetQualifiedName().
  const QualifiedName& GetQualifiedName() const { return name_; }
  // Ties the node to `element`, as setAttributeNode() does.
  void AttachToElement(Element& element);
  // Leaves its element, keeping `value`, as removing the attribute does.
  void DetachFromElementWithValue(const std::u16string& value);

 protected:
  Node* CloneWithoutChildren(Document& document) const override;

 private:
  cppgc::Member<Element> element_;
  const QualifiedName name_;
  // The value while it has no element.
  std::u16string standalone_value_;
};

}  // namespace mitto

#endif  // MITTO_DOM_ATTR_H_
