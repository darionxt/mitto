// The DOM's NamedNodeMap: an element's attributes as a live list of Attr
// nodes, by index and by qualified name.
//
// Upstream: third_party/blink/renderer/core/dom/named_node_map.h
// Upstream: third_party/blink/renderer/core/dom/named_node_map.cc

#ifndef MITTO_DOM_NAMED_NODE_MAP_H_
#define MITTO_DOM_NAMED_NODE_MAP_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class Attr;
class Element;

class NamedNodeMap final : public bindings::ScriptWrappable {
 public:
  static NamedNodeMap* Create(Element& element);

  explicit NamedNodeMap(Element& element);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  uint32_t Length() const;
  Attr* Item(uint32_t index) const;
  Attr* GetNamedItem(const std::u16string& name) const;
  Attr* GetNamedItemNS(const std::optional<std::u16string>& namespace_uri,
                       const std::u16string& local_name) const;
  Attr* SetNamedItem(Attr* attr, bindings::ExceptionState& exception_state);
  Attr* SetNamedItemNS(Attr* attr, bindings::ExceptionState& exception_state);
  Attr* RemoveNamedItem(const std::u16string& name,
                        bindings::ExceptionState& exception_state);
  Attr* RemoveNamedItemNS(const std::optional<std::u16string>& namespace_uri,
                          const std::u16string& local_name,
                          bindings::ExceptionState& exception_state);

  // The supported property names, the qualified names of the attributes in
  // order without the copies, and for an HTML element of an HTML document
  // without those that have an upper-case letter, as Blink's
  // NamedPropertyEnumerator() gives them.
  std::vector<std::u16string> NamedPropertyEnumerator() const;
  bool NamedPropertyQuery(const std::u16string& name) const;

 private:
  const cppgc::Member<Element> element_;
};

}  // namespace mitto

#endif  // MITTO_DOM_NAMED_NODE_MAP_H_
