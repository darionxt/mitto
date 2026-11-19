// Brings a Response that the Cache API matched its stored body, which Blink
// reads from the blob that the browser hands over with the response. Nothing
// of it comes until something reads the body, and then it comes as the reader
// takes it, as a fetch's body does, through a BodyLoaderBytesConsumer. The
// CacheStorageHost holds it until the body has come or is dropped.
//
// Upstream: third_party/blink/renderer/core/fetch/blob_bytes_consumer.cc

#ifndef MITTO_DOM_CACHE_BODY_LOADER_H_
#define MITTO_DOM_CACHE_BODY_LOADER_H_

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "mitto/dom/body_loader_bytes_consumer.h"
#include "mitto/dom/cache_storage_host.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-persistent-handle.h"

namespace mitto {

class CacheBodyLoader final : public StoredBodyClient, public BodyLoader {
 public:
  // Stored body `body` of `host`'s, read in `context`, whose window's
  // consumer `consumer()` gives.
  CacheBodyLoader(CacheStorageHost& host,
                  uint64_t body,
                  v8::Isolate* isolate,
                  v8::Local<v8::Context> context);
  CacheBodyLoader(const CacheBodyLoader&) = delete;
  CacheBodyLoader& operator=(const CacheBodyLoader&) = delete;
  ~CacheBodyLoader() override;

  // StoredBodyClient:
  void OnBodyData(std::string bytes) override;
  void OnBodyEnd(int net_error) override;

  // BodyLoader:
  void BodyRequested() override;
  void BodyTaken(size_t bytes) override;
  void BodyDropped() override;

  // What reads the body.
  BodyLoaderBytesConsumer* consumer() const { return consumer_.Get(); }

 private:
  void Release();

  const raw_ref<CacheStorageHost> host_;
  const uint64_t body_;
  const raw_ptr<v8::Isolate> isolate_;
  v8::Global<v8::Context> context_;
  // What reads the body, which goes with it.
  cppgc::WeakPersistent<BodyLoaderBytesConsumer> consumer_;
  bool started_ = false;
  base::WeakPtrFactory<CacheBodyLoader> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_DOM_CACHE_BODY_LOADER_H_
