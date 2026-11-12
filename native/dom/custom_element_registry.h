// The HTML Standard's CustomElementRegistry, the window's customElements,
// which document.customElementRegistry gives too: the object alone, with no
// custom elements. Its methods are Known Gaps.
//
// Upstream: third_party/blink/renderer/core/html/custom/custom_element_registry.cc

#ifndef MITTO_DOM_CUSTOM_ELEMENT_REGISTRY_H_
#define MITTO_DOM_CUSTOM_ELEMENT_REGISTRY_H_

#include "mitto/bindings/script_wrappable.h"

namespace mitto {

class CustomElementRegistry final : public bindings::ScriptWrappable {
 public:
  static CustomElementRegistry* Create();
  // The one a Known Gap gives, whose wrapper is of `info`.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  CustomElementRegistry();
  ~CustomElementRegistry() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
};

}  // namespace mitto

#endif  // MITTO_DOM_CUSTOM_ELEMENT_REGISTRY_H_
