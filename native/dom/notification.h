// Notifications API's Notification: what a page reads of it without showing
// one, its static maxActions. Its constructor, permission, requestPermission()
// and its other members are Known Gaps.
//
// Upstream: third_party/blink/renderer/modules/notifications/notification.cc
// Upstream: third_party/blink/public/common/notifications/notification_constants.h

#ifndef MITTO_DOM_NOTIFICATION_H_
#define MITTO_DOM_NOTIFICATION_H_

#include <stdint.h>

#include "mitto/dom/event_target.h"

namespace mitto {

class Notification final : public EventTarget {
 public:
  static Notification* Create();
  // The one a Known Gap gives, whose wrapper is of `info`.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  Notification();
  ~Notification() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface. kNotificationMaxActions.
  static uint32_t MaxActions() { return 2; }
};

}  // namespace mitto

#endif  // MITTO_DOM_NOTIFICATION_H_
