// The Presentation API's PresentationAvailability, which getAvailability()
// gives: whether a display to present to is there, which a Page's Machine
// never has.
//
// Upstream: third_party/blink/renderer/modules/presentation/presentation_availability.cc

#ifndef MITTO_DOM_PRESENTATION_AVAILABILITY_H_
#define MITTO_DOM_PRESENTATION_AVAILABILITY_H_

#include "mitto/dom/event_target.h"

namespace mitto {

class PresentationAvailability final : public EventTarget {
 public:
  static PresentationAvailability* Create();

  PresentationAvailability();
  ~PresentationAvailability() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface. No display ever comes, so its change event
  // never fires.
  bool Value() const { return false; }
};

}  // namespace mitto

#endif  // MITTO_DOM_PRESENTATION_AVAILABILITY_H_
