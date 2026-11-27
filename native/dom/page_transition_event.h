// The HTML Standard's PageTransitionEvent, which a window fires as pageshow
// once its document has loaded, and as pagehide when a navigation unloads it.
// persisted says whether the document goes to or comes from the back/forward
// cache, which a Page never has.
//
// Upstream: third_party/blink/renderer/core/events/page_transition_event.h
// Upstream: third_party/blink/renderer/core/events/page_transition_event.cc

#ifndef MITTO_DOM_PAGE_TRANSITION_EVENT_H_
#define MITTO_DOM_PAGE_TRANSITION_EVENT_H_

#include <string>

#include "base/time/time.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/event.h"

namespace mitto {

class PageTransitionEvent final : public Event {
 public:
  static PageTransitionEvent* Create(
      const std::u16string& type,
      const bindings::PageTransitionEventInit& init);
  // A trusted event that bubbles and can be canceled, as Blink's
  // PageTransitionEvent(type, persisted) makes it for a window.
  static PageTransitionEvent* CreateTrusted(const std::u16string& type,
                                            bool persisted);
  // PageTransitionEvent::CreatePersistedPageshow(): the pageshow of a page
  // that the back/forward cache restores, whose timeStamp is
  // `navigation_start`, the start of the traversal that restored it.
  static PageTransitionEvent* CreatePersistedPageshow(
      base::TimeTicks navigation_start);

  PageTransitionEvent(const std::u16string& type,
                      const bindings::PageTransitionEventInit& init);
  ~PageTransitionEvent() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  bool Persisted() const { return persisted_; }

 private:
  const bool persisted_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PAGE_TRANSITION_EVENT_H_
