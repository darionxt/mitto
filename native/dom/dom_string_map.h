// The DOM's DOMStringMap: an element's dataset, its data-* attributes as
// named properties in camel case, which setting and deleting change.
//
// Upstream: third_party/blink/renderer/core/dom/dom_string_map.h
// Upstream: third_party/blink/renderer/core/dom/dataset_dom_string_map.cc

#ifndef MITTO_DOM_DOM_STRING_MAP_H_
#define MITTO_DOM_DOM_STRING_MAP_H_

#include <optional>
#include <string>
#include <vector>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class Element;

class DOMStringMap final : public bindings::ScriptWrappable {
 public:
  static DOMStringMap* Create(Element& element);

  explicit DOMStringMap(Element& element);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The named property operations, as Blink's DatasetDOMStringMap has them.
  std::optional<std::u16string> AnonymousNamedGetter(
      const std::u16string& name) const;
  void AnonymousNamedSetter(const std::u16string& name,
                            const std::u16string& value,
                            bindings::ExceptionState& exception_state);
  // Whether it deleted an attribute: otherwise it leaves the property to an
  // ordinary delete, as Blink's kDidNotIntercept does.
  bool AnonymousNamedDeleter(const std::u16string& name);
  std::vector<std::u16string> NamedPropertyEnumerator() const;
  bool NamedPropertyQuery(const std::u16string& name) const;

 private:
  const cppgc::Member<Element> element_;
};

}  // namespace mitto

#endif  // MITTO_DOM_DOM_STRING_MAP_H_
