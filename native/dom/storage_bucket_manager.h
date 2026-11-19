// The Storage Buckets API's StorageBucketManager, navigator.storageBuckets:
// it opens, lists and deletes the buckets of the document's storage key,
// which its Session keeps beside the key's default bucket and every Page of
// the Session shares. Each promise settles from a task of its own once the
// Session has answered, and a name that is not valid, or a quota of 0,
// rejects without asking.
//
// Upstream: third_party/blink/renderer/modules/buckets/storage_bucket_manager.cc

#ifndef MITTO_DOM_STORAGE_BUCKET_MANAGER_H_
#define MITTO_DOM_STORAGE_BUCKET_MANAGER_H_

#include <string>

#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/script_wrappable.h"
#include "v8/include/v8-forward.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class StorageBucketManager final : public bindings::ScriptWrappable {
 public:
  static StorageBucketManager* Create();

  StorageBucketManager();
  ~StorageBucketManager() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  v8::Local<v8::Promise> Open(const std::u16string& name,
                              const bindings::StorageBucketOptions& options,
                              bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Keys(bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Delete(const std::u16string& name,
                                bindings::ExceptionState& exception_state);
};

}  // namespace mitto

#endif  // MITTO_DOM_STORAGE_BUCKET_MANAGER_H_
