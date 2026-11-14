// Storage's StorageManager, navigator.storage: what the Session's storage
// holds for the document's storage key and Chrome's fixed quota beside it, or
// for a document that its Session did not load no usage, and storage that is
// not persisted, which Chrome's durable storage permission does not grant
// without engagement. getDirectory() stays a Known Gap.
//
// Upstream: third_party/blink/renderer/modules/quota/storage_manager.cc
// Upstream: storage/browser/quota/quota_manager_impl.cc
// Upstream: storage/browser/quota/quota_features.cc
// Upstream: content/browser/storage_partition_impl.cc

#ifndef MITTO_DOM_STORAGE_MANAGER_H_
#define MITTO_DOM_STORAGE_MANAGER_H_

#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-promise.h"

namespace mitto {

class StorageManager final : public bindings::ScriptWrappable {
 public:
  static StorageManager* Create();

  StorageManager();
  ~StorageManager() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  v8::Local<v8::Promise> Estimate(bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Persisted(bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Persist(bindings::ExceptionState& exception_state);
};

}  // namespace mitto

#endif  // MITTO_DOM_STORAGE_MANAGER_H_
