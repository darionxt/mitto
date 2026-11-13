// The collections and lists that a node gives for getElementsByTagName(),
// getElementsByTagNameNS(), getElementsByClassName() and
// getElementsByName(), held weakly, so that the same call gives the same
// object while a page keeps it, as Blink's NodeListsNodeData caches them.
//
// Upstream: third_party/blink/renderer/core/dom/node_lists_node_data.h

#ifndef MITTO_DOM_NODE_LISTS_NODE_DATA_H_
#define MITTO_DOM_NODE_LISTS_NODE_DATA_H_

#include <optional>
#include <string>

#include "mitto/dom/html_collection.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class Node;
class NodeList;

class NodeListsNodeData final
    : public cppgc::GarbageCollected<NodeListsNodeData> {
 public:
  NodeListsNodeData();
  ~NodeListsNodeData();

  void Trace(cppgc::Visitor* visitor) const;

  HTMLCollection* TagCollection(Node& root, const std::u16string& name);
  HTMLCollection* TagCollectionNS(
      Node& root,
      const std::optional<std::u16string>& namespace_uri,
      const std::u16string& local_name);
  HTMLCollection* ClassCollection(Node& root,
                                  const std::u16string& class_names);
  HTMLCollection* DocumentAllNamedItems(Node& root, const std::u16string& name);
  NodeList* NameNodeList(Node& root, const std::u16string& name);

 private:
  struct Entry;

  // The collection of `type` made for `name` and `namespace_uri`, if a page
  // still keeps it. Drops the entries of those it no longer keeps.
  HTMLCollection* FindCollection(
      HTMLCollection::Type type,
      const std::u16string& name,
      const std::optional<std::u16string>& namespace_uri);
  void Add(HTMLCollection* collection, NodeList* list);

  cppgc::Member<Entry> first_;
};

}  // namespace mitto

#endif  // MITTO_DOM_NODE_LISTS_NODE_DATA_H_
