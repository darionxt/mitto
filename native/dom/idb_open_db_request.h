// IndexedDB's IDBOpenDBRequest, as Blink's: the request of an open or a
// delete, which hears blocked while other connections stay open, then
// upgradeneeded with the upgrade's transaction when the open asks for a newer
// version, and last success or error. A connection that closed before its
// success fires error instead.
//
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_open_db_request.h
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_open_db_request.cc

#ifndef MITTO_DOM_IDB_OPEN_DB_REQUEST_H_
#define MITTO_DOM_IDB_OPEN_DB_REQUEST_H_

#include <stdint.h>

#include <string>

#include "base/memory/raw_ptr.h"
#include "mitto/dom/idb_request.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class DOMException;
class IDBFactory;
class IndexedDbHost;
struct IDBDatabaseMetadata;

class IDBOpenDBRequest final : public IDBRequest {
 public:
  // An open of `name` at `version`, kNoVersion for none, whose connection
  // and upgrade transaction `host` numbered; or a delete, with no connection
  // and kDefaultVersion.
  IDBOpenDBRequest(Window* window,
                   IDBFactory* factory,
                   IndexedDbHost* host,
                   const std::u16string& name,
                   uint64_t connection,
                   uint64_t transaction_id,
                   int64_t version);
  ~IDBOpenDBRequest() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  const std::u16string& db_name() const { return db_name_; }
  int64_t version() const { return version_; }

  // What the Session answers, through IDBFactoryClient, in the window's
  // context. These fire their events at once, past the transaction's queue
  // of results, which no open waits behind.
  void OnBlocked(int64_t old_version);
  // Returns whether the request made the connection's IDBDatabase, which
  // otherwise closes.
  bool OnUpgradeNeeded(int64_t old_version,
                       const IDBDatabaseMetadata& metadata,
                       bool data_loss,
                       const std::u16string& data_loss_message);
  bool OnOpenDBSuccess(const IDBDatabaseMetadata& metadata);
  void OnDeleteDBSuccess(int64_t old_version);
  void OnDBFactoryError(DOMException* error);

 protected:
  bool CanStillSendResult() const override;

  // EventTarget:
  DispatchEventResult DispatchEventInternal(Event* event) override;

 private:
  const cppgc::Member<IDBFactory> factory_;
  const raw_ptr<IndexedDbHost> host_;
  const std::u16string db_name_;
  // An open's numbers, 0 for a delete.
  const uint64_t connection_;
  const uint64_t transaction_id_;
  int64_t version_;
};

}  // namespace mitto

#endif  // MITTO_DOM_IDB_OPEN_DB_REQUEST_H_
