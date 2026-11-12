#include "mitto/dom/html_all_collection.h"

#include "mitto/bindings/v8_html_all_collection.h"
#include "mitto/dom/document.h"
#include "mitto/dom/element.h"

namespace mitto {

// static
HTMLAllCollection* HTMLAllCollection::Create(Document* document) {
  return bindings::MakeGarbageCollected<HTMLAllCollection>(document);
}

// static
bindings::ScriptWrappable* HTMLAllCollection::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<HTMLAllCollection>(
      info, bindings::V8HTMLAllCollection::kWrapperTypeInfo, nullptr);
}

HTMLAllCollection::HTMLAllCollection(Document* document)
    : document_(document) {}

HTMLAllCollection::~HTMLAllCollection() = default;

const bindings::WrapperTypeInfo* HTMLAllCollection::GetWrapperTypeInfo() const {
  return &bindings::V8HTMLAllCollection::kWrapperTypeInfo;
}

void HTMLAllCollection::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(document_);
}

uint32_t HTMLAllCollection::Length() const {
  uint32_t length = 0;
  for (Node* node = document_ ? document_->FirstChild() : nullptr; node;
       node = node->NextInTree(document_.Get())) {
    if (node->IsElementNode()) {
      ++length;
    }
  }
  return length;
}

Element* HTMLAllCollection::AnonymousIndexedGetter(uint32_t index) const {
  for (Node* node = document_ ? document_->FirstChild() : nullptr; node;
       node = node->NextInTree(document_.Get())) {
    if (node->IsElementNode() && index-- == 0) {
      return static_cast<Element*>(node);
    }
  }
  return nullptr;
}

}  // namespace mitto
