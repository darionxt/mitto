#include "mitto/dom/html_template_element.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_html_template_element.h"
#include "mitto/dom/document.h"
#include "mitto/dom/document_fragment.h"

namespace mitto {

// static
HTMLTemplateElement* HTMLTemplateElement::Create(Document& document) {
  return bindings::MakeGarbageCollected<HTMLTemplateElement>(document);
}

HTMLTemplateElement::HTMLTemplateElement(Document& document)
    : HTMLElement(document, u"template") {}

const bindings::WrapperTypeInfo* HTMLTemplateElement::GetWrapperTypeInfo()
    const {
  return &bindings::V8HTMLTemplateElement::kWrapperTypeInfo;
}

void HTMLTemplateElement::Trace(cppgc::Visitor* visitor) const {
  HTMLElement::Trace(visitor);
  visitor->Trace(content_);
}

DocumentFragment* HTMLTemplateElement::Content() const {
  if (!content_) {
    content_ = DocumentFragment::Create(GetDocument().EnsureTemplateDocument());
  }
  return content_.Get();
}

void HTMLTemplateElement::CloningSteps(Node& copy, bool deep) const {
  // The HTML Standard's cloning steps: a deep copy gets copies of the
  // contents in its own.
  if (!deep || !content_) {
    return;
  }
  DocumentFragment* const contents =
      static_cast<HTMLTemplateElement&>(copy).Content();
  for (Node* child = content_->FirstChild(); child;
       child = child->NextSibling()) {
    contents->ParserAppendChild(child->Clone(contents->GetDocument(), true));
  }
}

void HTMLTemplateElement::DidMoveToNewDocument(Document& old_document) {
  HTMLElement::DidMoveToNewDocument(old_document);
  // The HTML Standard's adopting steps: the contents move to the template
  // document of the new document.
  if (content_) {
    content_->SetTreeDocument(GetDocument().EnsureTemplateDocument());
  }
}

}  // namespace mitto
