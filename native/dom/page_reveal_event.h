// The HTML Standard's PageRevealEvent: pagereveal, which a window's document
// gets at its first rendering opportunity, before the callbacks of that
// frame, as Blink's PageAnimator dispatches it. A Page has no view
// transitions, so viewTransition and the constructor, whose dictionary holds
// one, are Known Gaps.
//
// Upstream: third_party/blink/renderer/core/view_transition/page_reveal_event.h

#ifndef MITTO_DOM_PAGE_REVEAL_EVENT_H_
#define MITTO_DOM_PAGE_REVEAL_EVENT_H_

#include "mitto/dom/event.h"

namespace mitto {

class PageRevealEvent final : public Event {
 public:
  // A trusted pagereveal, which neither bubbles nor can be canceled.
  static PageRevealEvent* CreateTrusted();
  // One for a Known Gap that gives one.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  PageRevealEvent();
  ~PageRevealEvent() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_PAGE_REVEAL_EVENT_H_
