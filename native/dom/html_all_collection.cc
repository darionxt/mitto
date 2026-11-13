#include "mitto/dom/html_all_collection.h"

#include <algorithm>

#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_html_all_collection.h"
#include "mitto/dom/document.h"
#include "mitto/dom/element.h"
#include "mitto/dom/html_collection.h"
#include "mitto/dom/node_lists_node_data.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

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

// Blink's NamedGetter(), over the document's cached DocumentAllNameCollection.
bindings::ScriptWrappable* HTMLAllCollection::NamedGetter(
    const std::u16string& name) const {
  if (!document_) {
    return nullptr;
  }
  HTMLCollection* const items =
      document_->EnsureNodeLists().DocumentAllNamedItems(*document_, name);
  switch (items->Length()) {
    case 0:
      return nullptr;
    case 1:
      return items->Item(0);
    default:
      return items;
  }
}

bindings::ScriptWrappable* HTMLAllCollection::Item(
    const std::optional<v8::Local<v8::Value>>& name_or_index,
    bindings::ExceptionState& exception_state) const {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::Local<v8::Value> value =
      name_or_index ? *name_or_index : v8::Undefined(isolate).As<v8::Value>();
  v8::Local<v8::Uint32> index;
  if (value->ToArrayIndex(context).ToLocal(&index)) {
    return AnonymousIndexedGetter(index->Value());
  }
  std::u16string name;
  if (!bindings::FromV8String(isolate, value, &name)) {
    return nullptr;
  }
  return NamedGetter(name);
}

std::vector<std::u16string> HTMLAllCollection::NamedPropertyEnumerator() const {
  std::vector<std::u16string> names;
  const auto add = [&names](const std::u16string* name) {
    if (name && !name->empty() &&
        std::find(names.begin(), names.end(), *name) == names.end()) {
      names.push_back(*name);
    }
  };
  for (Node* node = document_ ? document_->FirstChild() : nullptr; node;
       node = node->NextInTree(document_.Get())) {
    if (!node->IsElementNode()) {
      continue;
    }
    const auto& element = static_cast<const Element&>(*node);
    add(element.FindAttribute(u"id"));
    if (IsAllNamedElement(element)) {
      add(element.FindAttribute(u"name"));
    }
  }
  return names;
}

bool HTMLAllCollection::NamedPropertyQuery(const std::u16string& name) const {
  return NamedGetter(name) != nullptr;
}

}  // namespace mitto
