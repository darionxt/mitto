// The Storage Buckets API's StorageBucket: a bucket of the document's storage
// key that StorageBucketManager opened, with an IndexedDB and a Cache API of
// its own, and its policies, which its Session keeps. Each object names its
// bucket by the number that the Session gave it, which goes once the object
// does. Its getDirectory() stays a Known Gap, as navigator.storage's does.
//
// Upstream: third_party/blink/renderer/modules/buckets/storage_bucket.cc

#ifndef MITTO_DOM_STORAGE_BUCKET_H_
#define MITTO_DOM_STORAGE_BUCKET_H_

#include <stdint.h>

#include <string>

#include "base/memory/raw_ptr.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/storage_bucket_messages.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/prefinalizer.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class CacheStorage;
class IDBFactory;
class StorageBucketHost;
class ExecutionContext;

class StorageBucket final : public bindings::ScriptWrappable {
  CPPGC_USING_PRE_FINALIZER(StorageBucket, Dispose);

 public:
  // Bucket `bucket` of `host`'s, named `name`, which `window`'s
  // StorageBucketManager opened.
  StorageBucket(ExecutionContext* context,
                StorageBucketHost* host,
                std::u16string name,
                uint64_t bucket);
  ~StorageBucket() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The number that the Session gave the bucket, which its IndexedDB and Cache
  // API name.
  uint64_t bucket() const { return bucket_; }

  // The members of the interface.
  std::u16string Name() const { return name_; }
  v8::Local<v8::Promise> Persist();
  v8::Local<v8::Promise> Persisted();
  v8::Local<v8::Promise> Estimate();
  v8::Local<v8::Promise> Durability();
  v8::Local<v8::Promise> SetExpires(double expires);
  v8::Local<v8::Promise> Expires();
  IDBFactory* IndexedDB();
  CacheStorage* Caches(bindings::ExceptionState& exception_state);

 private:
  // How an answer settles the promise of a call.
  using Settle = void (*)(v8::Isolate* isolate,
                          v8::Local<v8::Context> context,
                          v8::Local<v8::Promise::Resolver> resolver,
                          const PageStorageBucketAnswer& answer);

  // The Session no longer needs to keep the bucket for this object.
  void Dispose();
  // The document's host, or null once the window's context has gone, when
  // Blink's remote is unbound and each call rejects.
  StorageBucketHost* Host() const;
  v8::Local<v8::Promise> Ask(PageStorageBucketCall call, Settle settle);

  const cppgc::WeakMember<ExecutionContext> context_;
  const raw_ptr<StorageBucketHost> host_;
  const std::u16string name_;
  const uint64_t bucket_;
  cppgc::Member<IDBFactory> indexed_db_;
  cppgc::Member<CacheStorage> caches_;
};

}  // namespace mitto

#endif  // MITTO_DOM_STORAGE_BUCKET_H_
