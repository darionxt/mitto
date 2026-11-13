#include "mitto/dom/document_fragment.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_document_fragment.h"
#include "mitto/dom/document.h"
#include "mitto/dom/element.h"
#include "mitto/dom/window.h"

namespace mitto {

// static
DocumentFragment* DocumentFragment::Create() {
  return Create(Window::Current().GetDocument());
}

// static
DocumentFragment* DocumentFragment::Create(Document& document) {
  return bindings::MakeGarbageCollected<DocumentFragment>(document);
}

DocumentFragment::DocumentFragment(Document& document) : Node(&document) {}

const bindings::WrapperTypeInfo* DocumentFragment::GetWrapperTypeInfo() const {
  return &bindings::V8DocumentFragment::kWrapperTypeInfo;
}

Element* DocumentFragment::GetElementById(const std::u16string& id) const {
  if (id.empty()) {
    return nullptr;
  }
  for (Node* node = FirstChild(); node; node = node->NextInTree(this)) {
    if (!node->IsElementNode()) {
      continue;
    }
    auto* const element = static_cast<Element*>(node);
    if (const std::u16string* value = element->FindAttribute(u"id");
        value && *value == id) {
      return element;
    }
  }
  return nullptr;
}

uint16_t DocumentFragment::GetNodeType() const {
  return kDocumentFragmentNode;
}

std::u16string DocumentFragment::NodeName() const {
  return u"#document-fragment";
}

Node* DocumentFragment::CloneWithoutChildren(Document& document) const {
  return Create(document);
}

}  // namespace mitto
