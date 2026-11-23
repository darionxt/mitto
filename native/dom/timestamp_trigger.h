// Notification Triggers' TimestampTrigger, which a Page has only with the
// NotificationTriggers feature on: the object alone, so that a notification's
// showTrigger converts as Blink converts it. Its constructor and timestamp are
// Known Gaps.
//
// Upstream: third_party/blink/renderer/modules/notifications/timestamp_trigger.cc

#ifndef MITTO_DOM_TIMESTAMP_TRIGGER_H_
#define MITTO_DOM_TIMESTAMP_TRIGGER_H_

#include "mitto/bindings/script_wrappable.h"

namespace mitto {

class TimestampTrigger final : public bindings::ScriptWrappable {
 public:
  static TimestampTrigger* Create();
  // The one a Known Gap gives, whose wrapper is of `info`.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  TimestampTrigger();
  ~TimestampTrigger() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
};

}  // namespace mitto

#endif  // MITTO_DOM_TIMESTAMP_TRIGGER_H_
