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

#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/event.h"

namespace mitto {

class PageTransitionEvent final : public Event {
 public:
  static PageTransitionEvent* Create(
      const std::u16string& type,
      const bindings::PageTransitionEventInit& init);
  // A trusted event that neither bubbles nor can be canceled, as Blink's
  // document makes it for a window.
  static PageTransitionEvent* CreateTrusted(const std::u16string& type,
                                            bool persisted);

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
