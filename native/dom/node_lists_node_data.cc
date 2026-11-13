#include "mitto/dom/node_lists_node_data.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/document.h"
#include "mitto/dom/node.h"
#include "mitto/dom/node_list.h"

namespace mitto {

struct NodeListsNodeData::Entry final : public cppgc::GarbageCollected<Entry> {
  Entry(HTMLCollection* collection, NodeList* list, Entry* next)
      : collection(collection), list(list), next(next) {}

  void Trace(cppgc::Visitor* visitor) const {
    visitor->Trace(collection);
    visitor->Trace(list);
    visitor->Trace(next);
  }

  // One of them.
  cppgc::WeakMember<HTMLCollection> collection;
  cppgc::WeakMember<NodeList> list;
  cppgc::Member<Entry> next;
};

NodeListsNodeData::NodeListsNodeData() = default;
NodeListsNodeData::~NodeListsNodeData() = default;

void NodeListsNodeData::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(first_);
}

HTMLCollection* NodeListsNodeData::FindCollection(
    HTMLCollection::Type type,
    const std::u16string& name,
    const std::optional<std::u16string>& namespace_uri) {
  HTMLCollection* found = nullptr;
  cppgc::Member<Entry>* link = &first_;
  while (Entry* const entry = link->Get()) {
    if (!entry->collection && !entry->list) {
      *link = entry->next;
      continue;
    }
    HTMLCollection* const collection = entry->collection.Get();
    if (!found && collection && collection->type() == type &&
        collection->name() == name &&
        collection->namespace_uri() == namespace_uri) {
      found = collection;
    }
    link = &entry->next;
  }
  return found;
}

void NodeListsNodeData::Add(HTMLCollection* collection, NodeList* list) {
  first_ =
      bindings::MakeGarbageCollected<Entry>(collection, list, first_.Get());
}

HTMLCollection* NodeListsNodeData::TagCollection(Node& root,
                                                 const std::u16string& name) {
  const HTMLCollection::Type type =
      root.GetDocument().IsHTMLDocument()
          ? HTMLCollection::Type::kHTMLTagCollection
          : HTMLCollection::Type::kTagCollection;
  if (HTMLCollection* found = FindCollection(type, name, std::nullopt)) {
    return found;
  }
  HTMLCollection* const collection =
      HTMLCollection::CreateTagCollection(&root, name);
  Add(collection, nullptr);
  return collection;
}

HTMLCollection* NodeListsNodeData::TagCollectionNS(
    Node& root,
    const std::optional<std::u16string>& namespace_uri,
    const std::u16string& local_name) {
  if (HTMLCollection* found = FindCollection(
          HTMLCollection::Type::kTagCollectionNS, local_name, namespace_uri)) {
    return found;
  }
  HTMLCollection* const collection =
      HTMLCollection::CreateTagCollectionNS(&root, namespace_uri, local_name);
  Add(collection, nullptr);
  return collection;
}

HTMLCollection* NodeListsNodeData::ClassCollection(
    Node& root,
    const std::u16string& class_names) {
  if (HTMLCollection* found = FindCollection(
          HTMLCollection::Type::kClassCollection, class_names, std::nullopt)) {
    return found;
  }
  HTMLCollection* const collection =
      HTMLCollection::CreateClassCollection(&root, class_names);
  Add(collection, nullptr);
  return collection;
}

HTMLCollection* NodeListsNodeData::DocumentAllNamedItems(
    Node& root,
    const std::u16string& name) {
  if (HTMLCollection* found = FindCollection(
          HTMLCollection::Type::kDocumentAllNamedItems, name, std::nullopt)) {
    return found;
  }
  HTMLCollection* const collection =
      HTMLCollection::CreateDocumentAllNamedItems(&root, name);
  Add(collection, nullptr);
  return collection;
}

NodeList* NodeListsNodeData::NameNodeList(Node& root,
                                          const std::u16string& name) {
  for (Entry* entry = first_.Get(); entry; entry = entry->next.Get()) {
    NodeList* const list = entry->list.Get();
    if (list && list->type() == NodeList::Type::kName && list->name() == name) {
      return list;
    }
  }
  NodeList* const list = NodeList::CreateName(&root, name);
  Add(nullptr, list);
  return list;
}

}  // namespace mitto
