// Permissions' PermissionStatus: the state of a permission that
// navigator.permissions.query() gives, and its name as Blink writes it. A
// Page's states never change, so its change event never fires.
//
// Upstream: third_party/blink/renderer/modules/permissions/permission_status.h
// Upstream: third_party/blink/renderer/modules/permissions/permission_status.cc

#ifndef MITTO_DOM_PERMISSION_STATUS_H_
#define MITTO_DOM_PERMISSION_STATUS_H_

#include <string>

#include "mitto/dom/event_target.h"

namespace mitto {

class PermissionStatus final : public EventTarget {
 public:
  static PermissionStatus* Create(const std::u16string& name,
                                  const std::u16string& state);

  PermissionStatus(const std::u16string& name, const std::u16string& state);
  ~PermissionStatus() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  std::u16string Name() const { return name_; }
  std::u16string State() const { return state_; }

 private:
  const std::u16string name_;
  const std::u16string state_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PERMISSION_STATUS_H_
