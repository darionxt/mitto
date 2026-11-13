// The HTML Standard's UserActivation: whether a window has had a user's
// activation, and whether it has one now. A Page takes no input until M9, so
// both are false, as Chrome gives a page that nobody has touched: for
// navigator.userActivation and for the snapshot that postMessage() takes with
// includeUserActivation.
//
// Upstream: third_party/blink/renderer/core/frame/user_activation.h
// Upstream: third_party/blink/renderer/core/frame/user_activation.cc

#ifndef MITTO_DOM_USER_ACTIVATION_H_
#define MITTO_DOM_USER_ACTIVATION_H_

#include "mitto/bindings/script_wrappable.h"

namespace mitto {

class UserActivation final : public bindings::ScriptWrappable {
 public:
  // UserActivation::CreateSnapshot(): the window's state as it is now.
  static UserActivation* CreateSnapshot();
  // The one a Known Gap gives, such as navigator.userActivation.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  UserActivation(bool has_been_active, bool is_active);
  ~UserActivation() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  bool HasBeenActive() const { return has_been_active_; }
  bool IsActive() const { return is_active_; }

 private:
  const bool has_been_active_;
  const bool is_active_;
};

}  // namespace mitto

#endif  // MITTO_DOM_USER_ACTIVATION_H_
