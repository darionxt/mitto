// IndexedDB's IDBDatabase, a connection to a database, as Blink's: its
// metadata, its transactions, and what it hears of the Session, versionchange
// as another connection asks for a new version or a delete, a forced close,
// and the end of each transaction. close() waits for the transactions that
// run, and a connection that is closing takes no new one.
//
// The object store's, the index's and the cursor's parts of IndexedDB reach
// the database's metadata and the calls to the Session through the methods
// below "What the rest of IndexedDB uses".
//
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_database.h
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_database.cc

#ifndef MITTO_DOM_IDB_DATABASE_H_
#define MITTO_DOM_IDB_DATABASE_H_

#include <stdint.h>

#include <string>
#include <string_view>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/event_target.h"
#include "mitto/dom/heap_vector.h"
#include "mitto/dom/idb_metadata.h"
#include "mitto/dom/idb_request.h"
#include "mitto/indexed_db_key.h"
#include "mitto/indexed_db_messages.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/cppgc/prefinalizer.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class DOMStringList;
class IDBObjectStore;
class IDBTransaction;
class IndexedDbHost;
class Window;

class IDBDatabase final : public EventTarget {
  CPPGC_USING_PRE_FINALIZER(IDBDatabase, Dispose);

 public:
  // Blink's messages, which the rest of IndexedDB throws too.
  static constexpr char kIndexDeletedErrorMessage[] =
      "The index or its object store has been deleted.";
  static constexpr char kIndexNameTakenErrorMessage[] =
      "An index with the specified name already exists.";
  static constexpr char kIsKeyCursorErrorMessage[] =
      "The cursor is a key cursor.";
  static constexpr char kNoKeyOrKeyRangeErrorMessage[] =
      "No key or key range specified.";
  static constexpr char kNoSuchIndexErrorMessage[] =
      "The specified index was not found.";
  static constexpr char kNoSuchObjectStoreErrorMessage[] =
      "The specified object store was not found.";
  static constexpr char kNoValueErrorMessage[] =
      "The cursor is being iterated or has iterated past its end.";
  static constexpr char kNotValidKeyErrorMessage[] =
      "The parameter is not a valid key.";
  static constexpr char kNotVersionChangeTransactionErrorMessage[] =
      "The database is not running a version change transaction.";
  static constexpr char kObjectStoreDeletedErrorMessage[] =
      "The object store has been deleted.";
  static constexpr char kObjectStoreNameTakenErrorMessage[] =
      "An object store with the specified name already exists.";
  static constexpr char kRequestNotFinishedErrorMessage[] =
      "The request has not finished.";
  static constexpr char kSourceDeletedErrorMessage[] =
      "The cursor's source or effective object store has been deleted.";
  static constexpr char kTransactionInactiveErrorMessage[] =
      "The transaction is not active.";
  static constexpr char kTransactionFinishedErrorMessage[] =
      "The transaction has finished.";
  static constexpr char kTransactionReadOnlyErrorMessage[] =
      "The transaction is read-only.";
  static constexpr char kDatabaseClosedErrorMessage[] =
      "The database connection is closed.";

  // Connection `connection` of `window`'s document, which `host` gave and
  // the Session opened, whose events it hears from now on.
  IDBDatabase(Window* window, IndexedDbHost* host, uint64_t connection);
  ~IDBDatabase() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  std::u16string Name() const { return metadata_.name; }
  uint64_t Version() const { return static_cast<uint64_t>(metadata_.version); }
  DOMStringList* ObjectStoreNames() const;
  IDBTransaction* Transaction(
      const bindings::DOMStringOrDOMStringSequence& store_names,
      const std::u16string& mode,
      const bindings::IDBTransactionOptions& options,
      bindings::ExceptionState& exception_state);
  void Close();
  IDBObjectStore* CreateObjectStore(
      const std::u16string& name,
      const bindings::IDBObjectStoreParameters& options,
      bindings::ExceptionState& exception_state);
  void DeleteObjectStore(const std::u16string& name,
                         bindings::ExceptionState& exception_state);

  // What the rest of IndexedDB uses.
  //
  // The whole metadata, the object stores' and indexes' among it, as an open
  // gives it.
  void SetMetadata(const IDBDatabaseMetadata& metadata);
  // The database's own metadata alone, as an aborted upgrade puts it back.
  void SetDatabaseMetadata(const IDBDatabaseMetadata& metadata);
  const IDBDatabaseMetadata& Metadata() const { return metadata_; }
  int64_t FindObjectStoreId(const std::u16string& name) const;
  bool ContainsObjectStore(const std::u16string& name) const {
    return FindObjectStoreId(name) != IDBObjectStoreMetadata::kInvalidId;
  }
  // IDBObjectStore's name setter, in the upgrade.
  void RenameObjectStore(int64_t object_store_id,
                         const std::u16string& new_name);
  // An aborted upgrade's.
  void RevertObjectStoreCreation(int64_t object_store_id);
  void RevertObjectStoreMetadata(
      scoped_refptr<IDBObjectStoreMetadata> old_metadata);

  void TransactionCreated(IDBTransaction* transaction);
  // An upgrade transaction that is about to finish is the connection's no
  // more.
  void TransactionWillFinish(const IDBTransaction* transaction);
  // After the transaction's last event.
  void TransactionFinished(const IDBTransaction* transaction);

  bool IsClosePending() const { return close_pending_; }
  // Whether the Session's connection is still the page's.
  bool IsConnectionOpen() const { return connection_open_; }
  uint64_t Connection() const { return connection_; }
  IndexedDbHost* Host() const { return host_; }
  Window* GetWindow() const { return window_.Get(); }
  bool HasContext() const;

  // What the Session tells the connection, from a task of the window's
  // context: blink.mojom.IDBDatabaseCallbacks.
  void ForcedClose();
  void VersionChange(int64_t old_version, int64_t new_version);
  void Abort(uint64_t transaction_id,
             IndexedDbException code,
             const std::u16string& message);
  void Complete(uint64_t transaction_id);

  // What the connection tells the Session: blink.mojom.IDBDatabase. Nothing
  // goes once the connection has closed.
  void RenameObjectStore(uint64_t transaction_id,
                         int64_t object_store_id,
                         const std::u16string& new_name);
  void CreateTransaction(uint64_t transaction_id,
                         const std::vector<int64_t>& object_store_ids,
                         IndexedDbTransactionMode mode,
                         IndexedDbDurability durability);
  void VersionChangeIgnored();
  void Get(uint64_t transaction_id,
           int64_t object_store_id,
           int64_t index_id,
           IndexedDbKeyRange key_range,
           bool key_only,
           IDBRequest* request);
  void GetAll(uint64_t transaction_id,
              int64_t object_store_id,
              int64_t index_id,
              IndexedDbKeyRange key_range,
              IndexedDbGetAllResultType result_type,
              uint32_t max_count,
              IndexedDbCursorDirection direction,
              IDBRequest* request);
  // Opens a cursor for `request`, an openCursor()'s or openKeyCursor()'s, of
  // its transaction, whose answer goes to the request's OnOpenCursor(). Mojo
  // hands the cursor's remote over in the answer; here the Page numbers the
  // Session's cursor in the call.
  void OpenCursor(int64_t object_store_id,
                  int64_t index_id,
                  IndexedDbKeyRange key_range,
                  IndexedDbCursorDirection direction,
                  bool key_only,
                  IndexedDbTaskType task_type,
                  IDBRequest* request);
  void Count(uint64_t transaction_id,
             int64_t object_store_id,
             int64_t index_id,
             IndexedDbKeyRange key_range,
             IDBRequest* request);
  void Delete(uint64_t transaction_id,
              int64_t object_store_id,
              const IDBKey& primary_key,
              IDBRequest* request);
  void DeleteRange(uint64_t transaction_id,
                   int64_t object_store_id,
                   IndexedDbKeyRange key_range,
                   IDBRequest* request);
  void GetKeyGeneratorCurrentNumber(uint64_t transaction_id,
                                    int64_t object_store_id,
                                    IDBRequest* request);
  void Clear(uint64_t transaction_id,
             int64_t object_store_id,
             IDBRequest* request);
  void CreateIndex(uint64_t transaction_id,
                   int64_t object_store_id,
                   int64_t index_id,
                   const std::u16string& name,
                   const IDBKeyPath& key_path,
                   bool unique,
                   bool multi_entry);
  void DeleteIndex(uint64_t transaction_id,
                   int64_t object_store_id,
                   int64_t index_id);
  void RenameIndex(uint64_t transaction_id,
                   int64_t object_store_id,
                   int64_t index_id,
                   const std::u16string& new_name);
  void Abort(uint64_t transaction_id);
  void DidBecomeInactive();

  // Asks the Session `call` of this connection for `request`, and hands each
  // answer to the request's `on_answer` in its window's context, unless the
  // request or the context has gone, as Blink binds an IDBRequest method
  // weakly to a call's callback. Nothing goes once the connection has
  // closed.
  // IDBRequest is complete here, since a pointer to a member of an
  // incomplete class has another size in Microsoft's ABI.
  using RequestAnswer = void (IDBRequest::*)(const PageIndexedDbCallback&);
  void AskForRequest(PageIndexedDbCall call,
                     IDBRequest* request,
                     RequestAnswer on_answer);

 protected:
  // EventTarget:
  DispatchEventResult DispatchEventInternal(Event* event) override;
  void ListenersChanged(std::u16string_view type) override;

 private:
  IDBObjectStore* CreateObjectStore(const std::u16string& name,
                                    const IDBKeyPath& key_path,
                                    bool auto_increment,
                                    bindings::ExceptionState& exception_state);
  IDBTransaction* FindTransaction(uint64_t transaction_id) const;
  // IDBCursor::ResetCursorPrefetchCaches(): the cursors of transaction
  // `transaction_id` drop what they prefetched, before a call that could
  // change what it holds.
  void ResetCursorPrefetchCaches(uint64_t transaction_id);
  // Sends `call` of this connection, unless it has closed.
  void Tell(PageIndexedDbCall call);
  // What the Session told the connection.
  void OnConnectionCallback(PageIndexedDbCallback callback);
  // Drops the connection, which the Session closes, aborting what it runs.
  void CloseConnection();
  // The window's context went: the connection closes at once, without
  // waiting for its transactions.
  void ContextDestroyed();
  // A connection that the collector takes closes, as dropping Blink's remote
  // does.
  void Dispose();
  // HasPendingActivity(): an open connection that a page listens to stays,
  // to hear versionchange.
  void UpdateKeepAlive();

  const cppgc::WeakMember<Window> window_;
  const raw_ptr<IndexedDbHost> host_;
  const uint64_t connection_;
  bool connection_open_ = true;
  IDBDatabaseMetadata metadata_;
  cppgc::Member<IDBTransaction> version_change_transaction_;
  // In the order they were made.
  HeapVector<IDBTransaction> transactions_;
  bool close_pending_ = false;
  cppgc::Persistent<IDBDatabase> keep_alive_;
};

}  // namespace mitto

#endif  // MITTO_DOM_IDB_DATABASE_H_
