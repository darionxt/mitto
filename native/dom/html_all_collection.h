// The HTML Standard's HTMLAllCollection, document.all: a live view of every
// element of the document, in tree order, which is undetectable and can be
// called, as Blink's is. Its named properties, namedItem() and item(), whose
// results are a union, are Known Gaps.
//
// Upstream: third_party/blink/renderer/core/html/html_all_collection.cc

#ifndef MITTO_DOM_HTML_ALL_COLLECTION_H_
#define MITTO_DOM_HTML_ALL_COLLECTION_H_

#include <stdint.h>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class Document;
class Element;

class HTMLAllCollection final : public bindings::ScriptWrappable {
 public:
  static HTMLAllCollection* Create(Document* document);
  // An empty one, for a Known Gap that gives one.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  explicit HTMLAllCollection(Document* document);
  ~HTMLAllCollection() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  uint32_t Length() const;
  Element* AnonymousIndexedGetter(uint32_t index) const;

 private:
  cppgc::Member<Document> document_;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_ALL_COLLECTION_H_
