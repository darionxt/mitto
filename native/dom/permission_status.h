// Permissions' PermissionStatus: the state of a permission that
// navigator.permissions.query() gives, and its name as Blink writes it. A
// query gives its state to what earlier ones of the kind made, as they share
// Blink's PermissionStatusListener, with no change event, and nothing else
// changes a Page's states.
//
// Upstream: third_party/blink/renderer/modules/permissions/permission_status.h
// Upstream: third_party/blink/renderer/modules/permissions/permission_status.cc

#ifndef MITTO_DOM_PERMISSION_STATUS_H_
#define MITTO_DOM_PERMISSION_STATUS_H_

#include <string>

#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "mitto/dom/event_target.h"

namespace mitto {

class PermissionStatus final : public EventTarget {
 public:
  // The state of a kind of permission, as the latest query of it was answered.
  using SharedState = base::RefCountedData<std::u16string>;

  static PermissionStatus* Create(const std::u16string& name,
                                  scoped_refptr<SharedState> state);

  PermissionStatus(const std::u16string& name,
                   scoped_refptr<SharedState> state);
  ~PermissionStatus() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  std::u16string Name() const { return name_; }
  std::u16string State() const { return state_->data; }

 private:
  const std::u16string name_;
  const scoped_refptr<SharedState> state_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PERMISSION_STATUS_H_
