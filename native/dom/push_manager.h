// Push API's PushManager: what a page reads of it without a service worker,
// its static supportedContentEncodings. Its other members are Known Gaps.
//
// Upstream: third_party/blink/renderer/modules/push_messaging/push_manager.cc

#ifndef MITTO_DOM_PUSH_MANAGER_H_
#define MITTO_DOM_PUSH_MANAGER_H_

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-value.h"

namespace mitto {

class PushManager final : public bindings::ScriptWrappable {
 public:
  static PushManager* Create();
  // The one a Known Gap gives, whose wrapper is of `info`.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  PushManager();
  ~PushManager() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  static v8::Local<v8::Value> SupportedContentEncodings();
};

}  // namespace mitto

#endif  // MITTO_DOM_PUSH_MANAGER_H_
