// The HTML Standard's BarProp, the window's locationbar, menubar, personalbar,
// scrollbars, statusbar and toolbar: each is visible in a tab that a Session
// opened, as in a normal Chrome window, where only a popup opened without
// them hides them.
//
// Upstream: third_party/blink/renderer/core/frame/bar_prop.cc

#ifndef MITTO_DOM_BAR_PROP_H_
#define MITTO_DOM_BAR_PROP_H_

#include "mitto/bindings/script_wrappable.h"

namespace mitto {

class BarProp final : public bindings::ScriptWrappable {
 public:
  static BarProp* Create();
  // The one a Known Gap gives, whose wrapper is of `info`.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  BarProp();
  ~BarProp() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  bool Visible() const { return true; }
};

}  // namespace mitto

#endif  // MITTO_DOM_BAR_PROP_H_
