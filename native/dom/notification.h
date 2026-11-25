// Notifications API's Notification: what a page reads of it without showing
// one, its static maxActions, and permission and requestPermission() as Chrome
// answers them for a profile that has granted nothing, and its constructor,
// which runs Chrome's checks and fires error, since nobody has granted the
// permission. Its other members are Known Gaps.
//
// Upstream: third_party/blink/renderer/modules/notifications/notification.cc
// Upstream: third_party/blink/renderer/modules/notifications/notification_manager.cc
// Upstream: third_party/blink/renderer/modules/notifications/notification_data.cc
// Upstream: third_party/blink/public/common/notifications/notification_constants.h
// Upstream: components/permissions/permission_context_base.cc
// Upstream: chrome/browser/notifications/notification_permission_context.cc
// Upstream: components/content_settings/core/browser/content_settings_registry.cc

#ifndef MITTO_DOM_NOTIFICATION_H_
#define MITTO_DOM_NOTIFICATION_H_

#include <stdint.h>

#include <optional>
#include <string>

#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/dom/event_target.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-promise.h"

namespace mitto {

class Notification final : public EventTarget {
 public:
  static Notification* Create();
  // new Notification(title, options), as Blink's Notification::Create() and
  // CreateNotificationData() check `options`, with their messages, and
  // serialize its data for storage, which throws a DataCloneError for what
  // cannot be stored, such as a SharedArrayBuffer. The notification never
  // shows: SchedulePrepareShow() finds no permission, so error fires at it
  // from a task.
  static Notification* Create(const std::u16string& title,
                              const bindings::NotificationOptions& options,
                              bindings::ExceptionState& exception_state);
  // The one a Known Gap gives, whose wrapper is of `info`.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  Notification();
  ~Notification() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface. kNotificationMaxActions.
  static uint32_t MaxActions() { return 2; }
  // "default" on a secure page, the state of a permission that nobody has
  // granted or denied, and "denied" elsewhere, as permissions.query() says for
  // "notifications".
  static std::u16string Permission();
  // On a secure page Chrome shows its prompt, which waits for a person, so the
  // promise never settles. Elsewhere the answer is "denied", from a task of
  // its own, which goes to the deprecated callback first.
  static v8::Local<v8::Promise> RequestPermission(
      std::optional<v8::Local<v8::Function>> deprecated_callback);

 private:
  // HasPendingActivity(): one that a page made is never closed, since it never
  // shows, so it stays until its context goes.
  cppgc::Persistent<Notification> keep_alive_;
};

}  // namespace mitto

#endif  // MITTO_DOM_NOTIFICATION_H_
