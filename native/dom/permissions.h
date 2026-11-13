// Permissions, navigator.permissions: query() as Chrome answers it for a
// profile that has granted and denied nothing. The renderer reads the
// descriptor as Blink's ParsePermissionDescriptor() does, with its checks and
// messages; the state is the one that Chrome's browser gives each kind of
// permission then, on a secure page and elsewhere.
//
// Upstream: third_party/blink/renderer/modules/permissions/permissions.cc
// Upstream: third_party/blink/renderer/modules/permissions/permission_utils.cc
// Upstream: third_party/blink/renderer/modules/permissions/permission_descriptor.idl
// Upstream: third_party/blink/renderer/modules/permissions/permission_status_listener.cc
// Upstream: third_party/blink/common/permissions/permission_utils.cc
// Upstream: components/permissions/permission_context_base.cc
// Upstream: components/permissions/permission_manager.cc
// Upstream: components/content_settings/core/browser/content_settings_registry.cc
// Upstream: components/permissions/contexts/clipboard_sanitized_write_permission_context.cc
// Upstream: components/permissions/contexts/keyboard_lock_permission_context.cc
// Upstream: components/permissions/contexts/pointer_lock_permission_context.cc
// Upstream: components/permissions/contexts/wake_lock_permission_context.cc
// Upstream: chrome/browser/background_fetch/background_fetch_permission_context.cc
// Upstream: chrome/browser/background_sync/periodic_background_sync_permission_context.cc
// Upstream: chrome/browser/storage_access_api/storage_access_grant_permission_context.cc

#ifndef MITTO_DOM_PERMISSIONS_H_
#define MITTO_DOM_PERMISSIONS_H_

#include <map>
#include <string>

#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-promise.h"

namespace mitto {

class Permissions final : public bindings::ScriptWrappable {
 public:
  static Permissions* Create();

  Permissions();
  ~Permissions() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  v8::Local<v8::Promise> Query(v8::Local<v8::Object> permission,
                               bindings::ExceptionState& exception_state);

 private:
  // The name that each kind of permission gives its PermissionStatus: that
  // of the first descriptor the document asked about, as Blink keeps one
  // listener for each kind.
  std::map<int, std::u16string> names_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PERMISSIONS_H_
