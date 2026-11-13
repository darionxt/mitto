// The DOM's HTMLCollection: a live view of the elements under a node that a
// kind of collection takes, in tree order, as Blink's HTMLCollection and its
// subclasses read the tree. The document's images, embeds, links, forms,
// scripts, anchors and applets, a node's children, the rows, bodies and
// cells of a table, and what getElementsByTagName(),
// getElementsByTagNameNS() and getElementsByClassName() give. It keeps the
// elements it found until a tree changes (dom_tree_version.h), as Blink's
// collections keep their caches until they are invalidated.
//
// HTMLFormControlsCollection and HTMLOptionsCollection, which have no class,
// inherit from it, so it is not final.
//
// Upstream: third_party/blink/renderer/core/html/html_collection.cc
// Upstream: third_party/blink/renderer/core/dom/tag_collection.cc
// Upstream: third_party/blink/renderer/core/html/html_tag_collection.cc
// Upstream: third_party/blink/renderer/core/dom/class_collection.cc

#ifndef MITTO_DOM_HTML_COLLECTION_H_
#define MITTO_DOM_HTML_COLLECTION_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/heap_vector.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class Element;
class Node;

// Whether document.all names an element of this kind by its name attribute,
// as the HTML Standard's "all-named elements" are, and Blink's
// NameShouldBeVisibleInDocumentAll().
bool IsAllNamedElement(const Element& element);

class HTMLCollection : public bindings::ScriptWrappable {
 public:
  // The kinds of collection, as Blink's CollectionType has them.
  enum class Type {
    // Of nothing, as a Known Gap makes one.
    kEmpty,
    kDocImages,
    kDocEmbeds,
    kDocLinks,
    kDocForms,
    kDocScripts,
    kDocAnchors,
    // Always empty, as Chrome's has been since it dropped applets.
    kDocApplets,
    kChildren,
    // A table's rows, as Blink's HTMLTableRowsCollection orders them, its
    // tbody children, a section's tr children and a row's td and th children.
    kTableRows,
    kTBodies,
    kSectionRows,
    kRowCells,
    // getElementsByTagName() in a document that is not HTML.
    kTagCollection,
    // getElementsByTagName() in an HTML document, which matches the name in
    // lower case for an HTML element.
    kHTMLTagCollection,
    kTagCollectionNS,
    kClassCollection,
    // document.all's elements of one name or ID.
    kDocumentAllNamedItems,
  };

  static HTMLCollection* Create(Node* root, Type type);
  // getElementsByTagName()'s, of `qualified_name`.
  static HTMLCollection* CreateTagCollection(Node* root,
                                             const std::u16string& name);
  // getElementsByTagNameNS()'s. A null namespace is none, "*" any.
  static HTMLCollection* CreateTagCollectionNS(
      Node* root,
      const std::optional<std::u16string>& namespace_uri,
      const std::u16string& local_name);
  // getElementsByClassName()'s, of the classes in `class_names`.
  static HTMLCollection* CreateClassCollection(
      Node* root,
      const std::u16string& class_names);
  // The elements that document.all names `name`.
  static HTMLCollection* CreateDocumentAllNamedItems(
      Node* root,
      const std::u16string& name);
  // An empty one, for a Known Gap that gives one.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  HTMLCollection(Node* root, Type type);
  ~HTMLCollection() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  uint32_t Length() const;
  Element* Item(uint32_t index) const;
  Element* NamedItem(const std::u16string& name) const;

  // The supported property names: each element's ID, and its name for an
  // element of the HTML namespace, in tree order, each once.
  std::vector<std::u16string> NamedPropertyEnumerator() const;
  bool NamedPropertyQuery(const std::u16string& name) const;

  Type type() const { return type_; }
  // What the collection was made for: a tag name, local name, class names
  // or name, as NodeListsNodeData finds it again.
  const std::u16string& name() const { return name_; }
  const std::optional<std::u16string>& namespace_uri() const {
    return namespace_uri_;
  }

 private:
  bool Matches(const Element& element) const;
  // The elements, found again if a tree changed since the last time.
  const HeapVector<Element>& Elements() const;

  cppgc::Member<Node> root_;
  const Type type_;
  std::u16string name_;
  std::optional<std::u16string> namespace_uri_;
  // The classes of a class collection, in lower case in a document in
  // quirks mode.
  std::vector<std::u16string> class_names_;
  mutable HeapVector<Element> elements_;
  mutable std::optional<uint64_t> elements_version_;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_COLLECTION_H_
