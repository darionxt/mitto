#include "mitto/dom/push_manager.h"

#include "mitto/bindings/v8_push_manager.h"
#include "mitto/dom/script_values.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

// static
PushManager* PushManager::Create() {
  return bindings::MakeGarbageCollected<PushManager>();
}

// static
bindings::ScriptWrappable* PushManager::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<PushManager>(
      info, bindings::V8PushManager::kWrapperTypeInfo);
}

PushManager::PushManager() = default;
PushManager::~PushManager() = default;

const bindings::WrapperTypeInfo* PushManager::GetWrapperTypeInfo() const {
  return &bindings::V8PushManager::kWrapperTypeInfo;
}

// static
v8::Local<v8::Value> PushManager::SupportedContentEncodings() {
  return FrozenArrayOfStrings(v8::Isolate::GetCurrent(),
                              {u"aes128gcm", u"aesgcm"});
}

}  // namespace mitto
