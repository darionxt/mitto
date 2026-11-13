// The HTML Standard's HTMLAllCollection, document.all: a live view of every
// element of the document, in tree order, which is undetectable and can be
// called, as Blink's is. A name gives the one element of that ID, or of that
// name among the elements the HTML Standard names so, or an HTMLCollection of
// them when there are more.
//
// Upstream: third_party/blink/renderer/core/html/html_all_collection.cc

#ifndef MITTO_DOM_HTML_ALL_COLLECTION_H_
#define MITTO_DOM_HTML_ALL_COLLECTION_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-value.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

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
  // namedItem() and the named properties: an Element, an HTMLCollection or
  // null.
  bindings::ScriptWrappable* NamedGetter(const std::u16string& name) const;
  // An index, as V8 reads one, or else a name.
  bindings::ScriptWrappable* Item(
      const std::optional<v8::Local<v8::Value>>& name_or_index,
      bindings::ExceptionState& exception_state) const;

  // HTMLCollection's supported property names, which Blink's gives: each
  // element's ID, and its name for an HTML element, in tree order, each
  // once.
  std::vector<std::u16string> NamedPropertyEnumerator() const;
  bool NamedPropertyQuery(const std::u16string& name) const;

 private:
  cppgc::Member<Document> document_;
};

}  // namespace mitto

#endif  // MITTO_DOM_HTML_ALL_COLLECTION_H_
