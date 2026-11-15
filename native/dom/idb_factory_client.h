// What the Session answers an open or a delete, as Blink's IDBFactoryClient
// hears it for an IDBOpenDBRequest: blocked and upgradeneeded keep it
// listening, and success or an error ends it. A connection that an answer
// gives a request that can no longer take it, as its window's context went,
// closes, as dropping Blink's remote of it does.
//
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_factory_client.h
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_factory_client.cc

#ifndef MITTO_DOM_IDB_FACTORY_CLIENT_H_
#define MITTO_DOM_IDB_FACTORY_CLIENT_H_

#include <stdint.h>

#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "mitto/indexed_db_messages.h"
#include "v8/include/cppgc/persistent.h"

namespace mitto {

class IDBOpenDBRequest;
class IndexedDbHost;

class IDBFactoryClient final : public base::RefCounted<IDBFactoryClient> {
 public:
  // The client of `request`, an open whose connection `host` numbered
  // `connection`, or a delete, with 0.
  IDBFactoryClient(IDBOpenDBRequest* request,
                   IndexedDbHost* host,
                   uint64_t connection);
  IDBFactoryClient(const IDBFactoryClient&) = delete;
  IDBFactoryClient& operator=(const IDBFactoryClient&) = delete;

  // Each answer, from a task of its own.
  void OnAnswer(PageIndexedDbCallback answer);

 private:
  friend class base::RefCounted<IDBFactoryClient>;
  ~IDBFactoryClient();

  // Hands `answer` to the request, in its window's context, and returns
  // whether the request took the connection that it gives.
  bool Deliver(const PageIndexedDbCallback& answer);
  // The request hears nothing more.
  void Detach() { request_.Clear(); }

  // Held until its last answer, as Blink's client holds its request.
  cppgc::Persistent<IDBOpenDBRequest> request_;
  const raw_ptr<IndexedDbHost> host_;
  const uint64_t connection_;
  // Whether an answer gave the connection: upgradeneeded, or success without
  // an upgrade.
  bool connection_given_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_IDB_FACTORY_CLIENT_H_
