// The DOM's HTMLCollection: a live view of the elements under a node that a
// kind of collection takes, in tree order, as Blink's HTMLCollection and its
// document collections read the tree on each access. The document's images,
// embeds, links, forms, scripts, anchors and applets, a node's children, and
// the rows, bodies and cells of a table.
// HTMLFormControlsCollection and HTMLOptionsCollection, which have no class,
// inherit from it, so it is not final.
//
// Upstream: third_party/blink/renderer/core/html/html_collection.cc

#ifndef MITTO_DOM_HTML_COLLECTION_H_
#define MITTO_DOM_HTML_COLLECTION_H_

#include <stdint.h>

#include <string>
#include <vector>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class Element;
class Node;

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
  };

  static HTMLCollection* Create(Node* root, Type type);
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

 private:
  bool Matches(const Element& element) const;
  std::vector<Element*> Elements() const;

  cppgc::Member<Node> root_;
  const Type type_;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_COLLECTION_H_
