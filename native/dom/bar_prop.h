// The HTML Standard's BarProp, the window's locationbar, menubar, personalbar,
// scrollbars, statusbar and toolbar: each is visible in a tab that a Session
// opened, as in a normal Chrome window, where only a popup opened without
// them hides them, until its window's context goes, as a frame's that goes.
//
// Upstream: third_party/blink/renderer/core/frame/bar_prop.cc

#ifndef MITTO_DOM_BAR_PROP_H_
#define MITTO_DOM_BAR_PROP_H_

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class Window;

class BarProp final : public bindings::ScriptWrappable {
 public:
  // A bar of `window`.
  static BarProp* Create(Window& window);
  // The one a Known Gap gives, whose wrapper is of `info`.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  explicit BarProp(Window* window);
  ~BarProp() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface. BarProp::visible(): false once the window
  // has no context, and for none, as a Known Gap's bar has none.
  bool Visible() const;

 private:
  cppgc::WeakMember<Window> window_;
};

}  // namespace mitto

#endif  // MITTO_DOM_BAR_PROP_H_
