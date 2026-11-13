// The DOM's DocumentFragment: a node that holds children with no parent of
// its own, which inserting it gives to the parent, and a template element's
// contents.
//
// Upstream: third_party/blink/renderer/core/dom/document_fragment.h
// Upstream: third_party/blink/renderer/core/dom/document_fragment.cc

#ifndef MITTO_DOM_DOCUMENT_FRAGMENT_H_
#define MITTO_DOM_DOCUMENT_FRAGMENT_H_

#include <stdint.h>

#include <string>

#include "mitto/dom/node.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

class DocumentFragment : public Node {
 public:
  // new DocumentFragment(), in the current Window's document.
  static DocumentFragment* Create();
  static DocumentFragment* Create(Document& document);

  explicit DocumentFragment(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  Element* GetElementById(const std::u16string& id) const;

  // The template element whose contents this is, the host that the DOM
  // standard's host-including ancestors go through, as Blink's
  // TemplateContentDocumentFragment::Host().
  Element* Host() const { return host_.Get(); }
  void SetHost(Element* host) { host_ = host; }

  // Node's.
  uint16_t GetNodeType() const override;
  std::u16string NodeName() const override;

 protected:
  Node* CloneWithoutChildren(Document& document) const override;

 private:
  cppgc::Member<Element> host_;
};

}  // namespace mitto

#endif  // MITTO_DOM_DOCUMENT_FRAGMENT_H_
