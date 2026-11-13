// The template element, whose contents are a document fragment of an inert
// document of its own, which the parser fills and cloning and moving it carry
// along.
//
// Upstream: third_party/blink/renderer/core/html/html_template_element.h
// Upstream: third_party/blink/renderer/core/html/html_template_element.cc
// Upstream: third_party/blink/renderer/core/dom/template_content_document_fragment.h

#ifndef MITTO_DOM_HTML_TEMPLATE_ELEMENT_H_
#define MITTO_DOM_HTML_TEMPLATE_ELEMENT_H_

#include "mitto/dom/html_element.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

class DocumentFragment;

class HTMLTemplateElement final : public HTMLElement {
 public:
  static HTMLTemplateElement* Create(Document& document);

  explicit HTMLTemplateElement(Document& document);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface. The template contents, the same fragment
  // on every read, owned by the template document of the element's document.
  DocumentFragment* Content() const;

 protected:
  void CloningSteps(Node& copy, bool deep) const override;
  void DidMoveToNewDocument(Document& old_document) override;

 private:
  mutable cppgc::Member<DocumentFragment> content_;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_TEMPLATE_ELEMENT_H_
