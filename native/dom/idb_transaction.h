// IndexedDB's IDBTransaction, as Blink's: active while the task or the event
// that made it runs, inactive after, and committing once it is inactive with
// no request pending, or when commit() says so; aborting through abort(), an
// error or an uncaught exception in a request's listener, or the Session;
// then complete or abort at the transaction and its connection. A version
// change transaction keeps the metadata it began with, and puts it back if it
// aborts.
//
// The object store's part of IndexedDB reaches the stores of the transaction
// through ObjectStore() and the metadata methods, makes its requests through
// IDBRequest::Create(), and its writes through Put() and SetIndexKeys().
//
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_transaction.h
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_transaction.cc
// Upstream: third_party/blink/renderer/platform/scheduler/common/event_loop.cc

#ifndef MITTO_DOM_IDB_TRANSACTION_H_
#define MITTO_DOM_IDB_TRANSACTION_H_

#include <stdint.h>

#include <deque>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "mitto/dom/event_target.h"
#include "mitto/dom/heap_vector.h"
#include "mitto/dom/idb_metadata.h"
#include "mitto/indexed_db_key.h"
#include "mitto/indexed_db_messages.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class DOMException;
class DOMStringList;
class IDBCursor;
class IDBDatabase;
class IDBIndex;
class IDBObjectStore;
class IDBOpenDBRequest;
class IDBRequest;
class IDBRequestQueueItem;
class IndexedDbHost;
class Window;

class IDBTransaction final : public EventTarget {
 public:
  // A readonly or readwrite transaction of `database`'s `scope`, numbered
  // `id`, which the Session was told of. It is active until the microtask
  // checkpoint that comes next has run.
  static IDBTransaction* CreateNonVersionChange(
      Window* window,
      IndexedDbHost* host,
      uint64_t id,
      const std::set<std::u16string>& scope,
      IndexedDbTransactionMode mode,
      IndexedDbDurability durability,
      IDBDatabase* database);
  // The upgrade of `open_db_request`, numbered `id` by its open, whose
  // database had `old_metadata` before.
  static IDBTransaction* CreateVersionChange(
      Window* window,
      IndexedDbHost* host,
      uint64_t id,
      IDBDatabase* database,
      IDBOpenDBRequest* open_db_request,
      const IDBDatabaseMetadata& old_metadata);

  // For readonly and readwrite transactions.
  IDBTransaction(Window* window,
                 IndexedDbHost* host,
                 uint64_t id,
                 const std::set<std::u16string>& scope,
                 IndexedDbTransactionMode mode,
                 IndexedDbDurability durability,
                 IDBDatabase* database);
  // For upgrades.
  IDBTransaction(Window* window,
                 IndexedDbHost* host,
                 uint64_t id,
                 IDBDatabase* database,
                 IDBOpenDBRequest* open_db_request,
                 const IDBDatabaseMetadata& old_metadata);
  ~IDBTransaction() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  DOMStringList* ObjectStoreNames() const;
  std::u16string Mode() const;
  std::u16string Durability() const;
  IDBDatabase* Db() const { return database_.Get(); }
  DOMException* Error() const { return error_.Get(); }
  IDBObjectStore* ObjectStore(const std::u16string& name,
                              bindings::ExceptionState& exception_state);
  void Commit(bindings::ExceptionState& exception_state);
  void Abort(bindings::ExceptionState& exception_state);

  // What the rest of IndexedDB uses.
  uint64_t Id() const { return id_; }
  Window* GetWindow() const { return window_.Get(); }
  bool HasContext() const;
  IDBDatabase& db() const { return *database_; }
  IndexedDbTransactionMode GetMode() const { return mode_; }
  bool IsActive() const { return state_ == kActive; }
  bool IsFinished() const { return state_ == kFinished; }
  bool IsFinishing() const {
    return state_ == kCommitting || state_ == kAborting;
  }
  bool IsReadOnly() const {
    return mode_ == IndexedDbTransactionMode::kReadOnly;
  }
  bool IsVersionChange() const {
    return mode_ == IndexedDbTransactionMode::kVersionChange;
  }
  int64_t NumErrorsHandled() const { return num_errors_handled_; }
  void IncrementNumErrorsHandled() { ++num_errors_handled_; }

  // The requests that wait for their results.
  void RegisterRequest(IDBRequest* request);
  void UnregisterRequest(IDBRequest* request);

  // Whether a result waits in the queue, behind which new results queue up.
  bool HasQueuedResults() const { return !result_queue_.empty(); }
  void EnqueueResult(std::unique_ptr<IDBRequestQueueItem> result);
  // A result in the queue is ready: sends those at its front that are.
  void OnResultReady();

  // Called just before the database's metadata changes, which some of them
  // read first.
  void ObjectStoreCreated(const std::u16string& name,
                          IDBObjectStore* object_store);
  void ObjectStoreDeleted(int64_t object_store_id, const std::u16string& name);
  void ObjectStoreRenamed(const std::u16string& old_name,
                          const std::u16string& new_name);
  // An index that its object store deleted, whose metadata comes back if the
  // transaction aborts, unless the transaction made it.
  void IndexDeleted(IDBIndex* index);

  // Blink keeps a set of every cursor, which a call of a transaction goes
  // through; here each transaction keeps its own. A cursor joins when it is
  // made and leaves when it closes, and its transaction's calls that could
  // change what it prefetched drop that, but `except_cursor`'s.
  void RegisterCursor(IDBCursor* cursor);
  void UnregisterCursor(IDBCursor* cursor);
  void ResetCursorPrefetchCaches(IDBCursor* except_cursor);

  // Called around an event's dispatch, which may commit the transaction.
  void SetActive(bool new_is_active);
  // Called around a structured serialization, which may run a page's
  // getters, while which the transaction is inactive.
  void SetActiveDuringSerialization(bool new_is_active);
  // The first error is why it aborts.
  void SetError(DOMException* error);

  // The Session aborted or completed it.
  void OnAbort(DOMException* error);
  void OnComplete();
  // Starts aborting, unless it is already finishing: through abort(), or
  // because IndexedDB must, `from_frontend`, or because the Session did.
  void StartAborting(DOMException* error, bool from_frontend = true);

  // What the transaction tells the Session.
  void CreateObjectStore(int64_t object_store_id,
                         const std::u16string& name,
                         const IDBKeyPath& key_path,
                         bool auto_increment);
  void DeleteObjectStore(int64_t object_store_id);
  // Puts `value`, as SerializedScriptValue wrote it, with `primary_key` and
  // its index keys, and hands the answer to `request`'s OnPut(), or an error
  // at once for a put too large for the Session to take.
  void Put(int64_t object_store_id,
           std::string value,
           IDBKey primary_key,
           IndexedDbPutMode put_mode,
           std::vector<IndexedDbIndexKeys> index_keys,
           IDBRequest* request);
  void SetIndexKeys(int64_t object_store_id,
                    IDBKey primary_key,
                    IndexedDbIndexKeys index_keys);
  void SetIndexReady(int64_t object_store_id);

  // For IDBObjectStore::IsNewlyCreated(): the highest ID of an object store
  // before the upgrade.
  int64_t OldMaxObjectStoreId() const;
  // The message of a TransactionInactiveError: inactive, or finished.
  const char* InactiveErrorMessage() const;

  // The window's context went: nothing keeps the transaction any more.
  void ContextDestroyed();

 protected:
  // EventTarget:
  DispatchEventResult DispatchEventInternal(Event* event) override;

 private:
  enum State {
    // Made or started, but in no event's dispatch.
    kInactive,
    // Being made, or in an event's dispatch.
    kActive,
    kCommitting,
    kAborting,
    // No more events, and no more requests.
    kFinished,
  };

  // The object store of `name` that ObjectStore() gave, or null.
  IDBObjectStore* FindObjectStore(const std::u16string& name) const;
  void EraseObjectStore(const std::u16string& name);
  // The metadata that a version change transaction kept of `object_store`,
  // or null.
  scoped_refptr<IDBObjectStoreMetadata> OldStoreMetadata(
      const IDBObjectStore* object_store) const;

  void AbortOutstandingRequests(bool queue_tasks);
  void RevertDatabaseMetadata();
  // Called once the transaction has completed or aborted.
  void Finished();

  const cppgc::WeakMember<Window> window_;
  const raw_ptr<IndexedDbHost> host_;
  const uint64_t id_;
  const cppgc::Member<IDBDatabase> database_;
  cppgc::Member<IDBOpenDBRequest> open_db_request_;
  const IndexedDbTransactionMode mode_;
  const IndexedDbDurability durability_;
  // The names of the object stores of a readonly or readwrite transaction,
  // which cannot be renamed outside an upgrade. Empty for an upgrade, whose
  // scope is the whole database.
  const std::set<std::u16string> scope_;
  State state_;
  int64_t num_errors_handled_ = 0;
  cppgc::Member<DOMException> error_;
  // In the order they were made.
  HeapVector<IDBRequest> request_list_;
  // The results whose events have not fired yet.
  std::deque<std::unique_ptr<IDBRequestQueueItem>> result_queue_;
  // Whether OnResultReady() is running.
  bool handling_ready_ = false;
  // The object stores that ObjectStore() gave, by name, which it gives again,
  // as the spec has it.
  std::vector<std::u16string> object_store_names_;
  HeapVector<IDBObjectStore> object_stores_;
  // A version change transaction's: the metadata of each object store that
  // ObjectStore() gave, as it was then, ...
  HeapVector<IDBObjectStore> old_store_metadata_stores_;
  std::vector<scoped_refptr<IDBObjectStoreMetadata>> old_store_metadata_;
  // ... the metadata of the object stores it deleted that it gave none for,
  std::vector<scoped_refptr<IDBObjectStoreMetadata>> deleted_object_stores_;
  // ... the indexes it deleted that were there before it,
  HeapVector<IDBIndex> deleted_indexes_;
  // ... and the database's own metadata, without its object stores'.
  IDBDatabaseMetadata old_database_metadata_;
  // Its cursors, which the collector may take, as Blink's set holds them.
  WeakHeapVector<IDBCursor> cursors_;
  // HasPendingActivity(): the transaction stays until its complete or abort
  // event has fired, or its window's context goes.
  cppgc::Persistent<IDBTransaction> keep_alive_;
};

}  // namespace mitto

#endif  // MITTO_DOM_IDB_TRANSACTION_H_
