// IndexedDB as Chrome's browser runs it, for one Session: the databases of
// each storage key, their connections from every Page of the Session, the
// requests to open and delete them, with versionchange and blocked across
// connections, the transactions with their locks and task queues, and the
// cursors. Each Page asks through PageIndexedDb, and hears through the
// callbacks below, as a renderer asks Chrome's IDBFactory and IDBDatabase
// and hears through IDBFactoryClient and IDBDatabaseCallbacks.
//
// Locks follow Chrome's on-disk store: a transaction shares its database's
// lock and holds each object store of its scope shared or exclusive, so two
// readwrite transactions on other object stores run at once, and an upgrade
// holds the database alone.
//
// Upstream: content/browser/indexed_db/instance/bucket_context.cc
// Upstream: content/browser/indexed_db/instance/database.cc
// Upstream: content/browser/indexed_db/instance/connection_coordinator.cc
// Upstream: content/browser/indexed_db/instance/connection.cc
// Upstream: content/browser/indexed_db/instance/transaction.cc
// Upstream: content/browser/indexed_db/instance/cursor.cc

#ifndef MITTO_INDEXED_DB_ENGINE_H_
#define MITTO_INDEXED_DB_ENGINE_H_

#include <stddef.h>
#include <stdint.h>

#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/containers/queue.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "mitto/indexed_db/backing_store.h"
#include "mitto/indexed_db/lock_manager.h"
#include "mitto/indexed_db_messages.h"

namespace mitto {

class IndexedDbBucket;
class IndexedDbConnection;
class IndexedDbCursor;
class IndexedDbDatabase;
class IndexedDbTransaction;

// The answer to one call, which the engine fills, but for the number of the
// call, and the caller sends.
using IndexedDbReply = base::OnceCallback<void(PageIndexedDbCallback)>;

// blink.mojom.IDBFactoryClient, of one open() or deleteDatabase().
class IndexedDbFactoryClient {
 public:
  virtual ~IndexedDbFactoryClient() = default;

  // Whether the Page that asked is still there.
  virtual bool IsConnected() const = 0;

  virtual void Error(IndexedDbException code, std::u16string message) = 0;
  virtual void Blocked(int64_t old_version) = 0;
  virtual void UpgradeNeeded(std::unique_ptr<IndexedDbConnection> connection,
                             int64_t old_version,
                             const IndexedDbDatabaseMetadata& metadata) = 0;
  // `connection` is null after an upgrade, whose connection the client has.
  virtual void OpenSuccess(std::unique_ptr<IndexedDbConnection> connection,
                           const IndexedDbDatabaseMetadata& metadata) = 0;
  virtual void DeleteSuccess(int64_t old_version) = 0;
};

// blink.mojom.IDBDatabaseCallbacks, of one connection.
class IndexedDbDatabaseCallbacks {
 public:
  virtual ~IndexedDbDatabaseCallbacks() = default;

  virtual void ForcedClose() = 0;
  virtual void VersionChange(int64_t old_version, int64_t new_version) = 0;
  virtual void Abort(uint64_t transaction_id,
                     IndexedDbException code,
                     const std::u16string& message) = 0;
  virtual void Complete(uint64_t transaction_id) = 0;
  // What the Page sent of this connection is not what a renderer sends.
  virtual void BadMessage(const std::string& why) = 0;
};

// What the engine asks of the quota of its storage key.
class IndexedDbQuota {
 public:
  virtual ~IndexedDbQuota() = default;

  // Calls `done` with whether `bytes` more fit in the storage key's quota.
  virtual void CheckCanUseDiskSpace(int64_t bytes,
                                    base::OnceCallback<void(bool)> done) = 0;
  virtual void NotifyModified(int64_t delta) = 0;
};

// `reply`, which answers with an ignorable abort error, `op` its kind, if it
// goes unrun, as an operation that an aborted transaction drops does, and
// counts the error as one the transaction sent.
IndexedDbReply AbortOnDestruct(IndexedDbReply reply,
                               IndexedDbCallbackOp op,
                               base::WeakPtr<IndexedDbTransaction> transaction);
// `reply`, which answers `answer` if it goes unrun.
IndexedDbReply InvokeIfNotRun(IndexedDbReply reply,
                              PageIndexedDbCallback answer);

// An operation of `method` on `*target`, which does nothing once `target` is
// gone.
template <typename T, typename Method, typename... Args>
base::OnceCallback<bool(IndexedDbTransaction&)>
BindWeakOperation(Method method, base::WeakPtr<T> target, Args&&... args) {
  CHECK(target);
  T* const pointer = target.get();
  return base::BindOnce(
      [](base::WeakPtr<T> target,
         base::OnceCallback<bool(IndexedDbTransaction&)> operation,
         IndexedDbTransaction& transaction) {
        return target ? std::move(operation).Run(transaction) : true;
      },
      std::move(target),
      base::BindOnce(method, base::Unretained(pointer),
                     std::forward<Args>(args)...));
}

// Chrome's GetAllResultSinkWrapper: sends a getAll()'s records in chunks,
// and an error if it goes before it sent the last.
class IndexedDbGetAllSink {
 public:
  IndexedDbGetAllSink(base::RepeatingCallback<void(PageIndexedDbCallback)> sink,
                      base::WeakPtr<IndexedDbTransaction> transaction);
  IndexedDbGetAllSink(const IndexedDbGetAllSink&) = delete;
  IndexedDbGetAllSink& operator=(const IndexedDbGetAllSink&) = delete;
  ~IndexedDbGetAllSink();

  void SendResults(std::vector<IndexedDbRecord> records, bool done);
  void SendError(IndexedDbException code, std::u16string message);

 private:
  base::RepeatingCallback<void(PageIndexedDbCallback)> sink_;
  base::WeakPtr<IndexedDbTransaction> transaction_;
  bool done_ = false;
};

class IndexedDbTransaction {
 public:
  enum State {
    kCreated,
    kStarted,
    kCommitting,
    kFinished,
  };
  // An operation of the task queue: false for an error of the backing store,
  // which aborts the transaction.
  using Operation = base::OnceCallback<bool(IndexedDbTransaction&)>;

  IndexedDbTransaction(uint64_t id,
                       IndexedDbConnection& connection,
                       std::set<int64_t> scope,
                       IndexedDbTransactionMode mode,
                       IndexedDbDurability durability);
  IndexedDbTransaction(const IndexedDbTransaction&) = delete;
  IndexedDbTransaction& operator=(const IndexedDbTransaction&) = delete;
  ~IndexedDbTransaction();

  uint64_t id() const { return id_; }
  IndexedDbTransactionMode mode() const { return mode_; }
  // Where the backing store would flush its commit: no store here does yet.
  IndexedDbDurability durability() const { return durability_; }
  const std::set<int64_t>& scope() const { return scope_; }
  State state() const { return state_; }
  bool aborted() const { return aborted_; }
  bool IsAcceptingRequests() const {
    return state_ != kCommitting && state_ != kFinished && !is_commit_pending_;
  }
  IndexedDbConnection& connection() { return *connection_; }
  IndexedDbDatabase& database();
  IndexedDbBackingStore::Transaction& backing_store() {
    return *backing_store_;
  }
  IndexedDbLockHolder& locks_holder() { return locks_holder_; }
  base::WeakPtr<IndexedDbTransaction> AsWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  void ScheduleTask(IndexedDbTaskType type, Operation operation);
  void ScheduleTask(Operation operation) {
    ScheduleTask(IndexedDbTaskType::kNormal, std::move(operation));
  }
  // The locks are held: tasks may run.
  void Start();
  void Abort(IndexedDbException code, std::u16string message);
  void SetCommitFlag();
  void Commit(int64_t num_errors_handled);
  // Runs what waits in the task queues, and commits once nothing waits and
  // the Page asked to. Returns false after an error of the backing store.
  bool RunTasks();

  // blink.mojom.IDBTransaction.
  void CreateObjectStore(int64_t object_store_id,
                         std::u16string name,
                         IDBKeyPath key_path,
                         bool auto_increment);
  void DeleteObjectStore(int64_t object_store_id);
  void Put(int64_t object_store_id,
           std::string value,
           IDBKey key,
           IndexedDbPutMode mode,
           std::vector<IndexedDbIndexKeys> index_keys,
           IndexedDbReply reply);
  void SetIndexKeys(int64_t object_store_id,
                    IDBKey primary_key,
                    std::vector<IndexedDbIndexKeys> index_keys);
  void SetIndexKeysDone();
  void AddPreemptiveEvent() { ++pending_preemptive_events_; }

  void IncrementNumErrorsSent() { ++num_errors_sent_; }
  void RegisterOpenCursor(IndexedDbCursor* cursor);
  void UnregisterOpenCursor(IndexedDbCursor* cursor);
  // The IDs of the locks it holds.
  std::vector<IndexedDbLockId> lock_ids() const;

 private:
  bool HasPendingTasks() const;
  bool DoPendingCommit();
  void OnQuotaCheckDone(bool allowed);
  bool CommitPhaseTwo();
  void CloseOpenCursors();
  void OnInactivityTimeout();
  bool DoPut(int64_t object_store_id,
             std::string value,
             IDBKey key,
             IndexedDbPutMode mode,
             std::vector<IndexedDbIndexKeys> index_keys,
             IndexedDbReply reply,
             IndexedDbTransaction& transaction);

  const uint64_t id_;
  const raw_ref<IndexedDbConnection> connection_;
  const std::set<int64_t> scope_;
  const IndexedDbTransactionMode mode_;
  const IndexedDbDurability durability_;
  State state_ = kCreated;
  bool aborted_ = false;
  bool used_ = false;
  bool is_commit_pending_ = false;
  bool processing_ = false;
  int64_t num_errors_sent_ = 0;
  int64_t num_errors_handled_ = 0;
  int pending_preemptive_events_ = 0;
  // What the puts add, which the quota is checked against at commit.
  int64_t preliminary_size_estimate_ = 0;
  base::queue<Operation> task_queue_;
  base::queue<Operation> preemptive_task_queue_;
  IndexedDbLockHolder locks_holder_;
  std::unique_ptr<IndexedDbBackingStore::Transaction> backing_store_;
  // The metadata before an upgrade, which an abort brings back.
  std::optional<IndexedDbDatabaseMetadata> metadata_before_;
  std::set<raw_ptr<IndexedDbCursor>> open_cursors_;
  base::OneShotTimer timeout_timer_;
  base::WeakPtrFactory<IndexedDbTransaction> weak_factory_{this};
};

class IndexedDbConnection {
 public:
  IndexedDbConnection(IndexedDbDatabase& database,
                      std::unique_ptr<IndexedDbDatabaseCallbacks> callbacks,
                      uintptr_t client_token);
  IndexedDbConnection(const IndexedDbConnection&) = delete;
  IndexedDbConnection& operator=(const IndexedDbConnection&) = delete;
  // Closes the connection, as dropping its remote does.
  ~IndexedDbConnection();

  bool IsConnected() const { return callbacks_ != nullptr; }
  IndexedDbDatabase& database() { return *database_; }
  IndexedDbDatabaseCallbacks* callbacks() { return callbacks_.get(); }
  uintptr_t client_token() const { return client_token_; }
  const std::map<uint64_t, std::unique_ptr<IndexedDbTransaction>>&
  transactions() const {
    return transactions_;
  }
  base::WeakPtr<IndexedDbConnection> AsWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  IndexedDbTransaction* GetTransaction(uint64_t id) const;
  // The transaction if it may take a request now, or nullopt with what a
  // request of it gets instead. Sets `bad` for one that no renderer would
  // ask with.
  IndexedDbTransaction* GetTransactionAndVerifyState(
      uint64_t id,
      std::optional<IndexedDbTransactionMode> required_mode,
      IndexedDbException& code,
      std::u16string& message,
      bool& bad);
  IndexedDbTransaction* CreateVersionChangeTransaction(uint64_t id);

  // blink.mojom.IDBDatabase. A call that no renderer makes refuses the Page.
  void CreateTransaction(uint64_t id,
                         const std::vector<int64_t>& scope,
                         IndexedDbTransactionMode mode,
                         IndexedDbDurability durability);
  void RenameObjectStore(uint64_t transaction_id,
                         int64_t object_store_id,
                         std::u16string new_name);
  void VersionChangeIgnored();
  void Get(uint64_t transaction_id,
           int64_t object_store_id,
           int64_t index_id,
           IndexedDbKeyRange key_range,
           bool key_only,
           IndexedDbReply reply);
  // `sink` hears each chunk of records, the last one done.
  void GetAll(uint64_t transaction_id,
              int64_t object_store_id,
              int64_t index_id,
              IndexedDbKeyRange key_range,
              IndexedDbGetAllResultType result_type,
              uint32_t max_count,
              IndexedDbCursorDirection direction,
              base::RepeatingCallback<void(PageIndexedDbCallback)> sink);
  // `adopt_cursor` takes the cursor it opens, before `reply` runs.
  void OpenCursor(
      uint64_t transaction_id,
      int64_t object_store_id,
      int64_t index_id,
      IndexedDbKeyRange key_range,
      IndexedDbCursorDirection direction,
      bool key_only,
      IndexedDbTaskType task_type,
      base::OnceCallback<void(std::unique_ptr<IndexedDbCursor>)> adopt_cursor,
      IndexedDbReply reply);
  void Count(uint64_t transaction_id,
             int64_t object_store_id,
             int64_t index_id,
             IndexedDbKeyRange key_range,
             IndexedDbReply reply);
  void DeleteRange(uint64_t transaction_id,
                   int64_t object_store_id,
                   IndexedDbKeyRange key_range,
                   IndexedDbReply reply);
  void GetKeyGeneratorCurrentNumber(uint64_t transaction_id,
                                    int64_t object_store_id,
                                    IndexedDbReply reply);
  void Clear(uint64_t transaction_id,
             int64_t object_store_id,
             IndexedDbReply reply);
  void CreateIndex(uint64_t transaction_id,
                   int64_t object_store_id,
                   IndexedDbIndexMetadata index);
  void DeleteIndex(uint64_t transaction_id,
                   int64_t object_store_id,
                   int64_t index_id);
  void RenameIndex(uint64_t transaction_id,
                   int64_t object_store_id,
                   int64_t index_id,
                   std::u16string new_name);
  void Abort(uint64_t transaction_id);

  void RemoveTransaction(uint64_t id);
  void AbortAllTransactions(IndexedDbException code,
                            const std::u16string& message);
  // Aborts the transactions and closes, and returns the callbacks, which go.
  std::unique_ptr<IndexedDbDatabaseCallbacks> AbortTransactionsAndClose(
      const std::string& message);
  void CloseAndReportForceClose(const std::string& message);
  bool IsHoldingLocks(const std::vector<IndexedDbLockId>& lock_ids) const;
  // Refuses the Page: what it sent is not what a renderer sends.
  void ReportBadMessage(const std::string& why);
  // GetTransactionAndVerifyState(), which refuses the Page for a call that
  // no renderer makes, and fills in what the call gets instead.
  IndexedDbTransaction* VerifyTransaction(
      uint64_t id,
      std::optional<IndexedDbTransactionMode> required_mode,
      IndexedDbException* code = nullptr,
      std::u16string* message = nullptr);
  // A closure to run when the connection closes while it upgrades.
  void SetOnCloseDuringUpgrade(base::OnceClosure on_close) {
    on_close_during_upgrade_ = std::move(on_close);
  }

 private:
  // The database, which outlives the connection while it is connected.
  const base::WeakPtr<IndexedDbDatabase> database_;
  std::unique_ptr<IndexedDbDatabaseCallbacks> callbacks_;
  const uintptr_t client_token_;
  std::map<uint64_t, std::unique_ptr<IndexedDbTransaction>> transactions_;
  base::OnceClosure on_close_during_upgrade_;
  base::WeakPtrFactory<IndexedDbConnection> weak_factory_{this};
};

class IndexedDbCursor {
 public:
  IndexedDbCursor(std::unique_ptr<IndexedDbBackingStore::Cursor> cursor,
                  bool is_index,
                  IndexedDbCursorDirection direction,
                  bool key_only,
                  IndexedDbTaskType task_type,
                  base::WeakPtr<IndexedDbTransaction> transaction);
  IndexedDbCursor(const IndexedDbCursor&) = delete;
  IndexedDbCursor& operator=(const IndexedDbCursor&) = delete;
  ~IndexedDbCursor();

  // blink.mojom.IDBCursor. Each returns false for a call that no renderer
  // makes.
  bool Advance(uint32_t count, IndexedDbReply reply);
  bool Continue(IDBKey key, IDBKey primary_key, IndexedDbReply reply);
  void Prefetch(uint32_t count, IndexedDbReply reply);
  bool PrefetchReset(uint32_t used_prefetches);
  void Close();

  // The value at the cursor, which the open cursor's answer carries.
  PageIndexedDbCallback CurrentEntry();

 private:
  bool AdvanceOperation(uint32_t count,
                        IndexedDbReply reply,
                        IndexedDbTransaction& transaction);
  bool ContinueOperation(IDBKey key,
                         IDBKey primary_key,
                         IndexedDbReply reply,
                         IndexedDbTransaction& transaction);
  bool PrefetchOperation(uint32_t count,
                         IndexedDbReply reply,
                         IndexedDbTransaction& transaction);
  // Whether the cursor may take a call, or else answers `reply` that it is
  // closed.
  bool CheckOpen(IndexedDbReply& reply);

  std::unique_ptr<IndexedDbBackingStore::Cursor> cursor_;
  const bool is_index_;
  const IndexedDbCursorDirection direction_;
  const bool key_only_;
  const IndexedDbTaskType task_type_;
  base::WeakPtr<IndexedDbTransaction> transaction_;
  bool closed_ = false;
  bool reached_end_during_prefetch_ = false;
  // Where the first entry of the last prefetch is, which a reset goes back
  // to.
  std::optional<IndexedDbBackingStore::Cursor::Position> saved_position_;
  base::WeakPtrFactory<IndexedDbCursor> weak_factory_{this};
};

class IndexedDbDatabase {
 public:
  IndexedDbDatabase(std::u16string name, IndexedDbBucket& bucket);
  IndexedDbDatabase(const IndexedDbDatabase&) = delete;
  IndexedDbDatabase& operator=(const IndexedDbDatabase&) = delete;
  ~IndexedDbDatabase();

  const std::u16string& name() const { return name_; }
  int64_t id() const { return id_; }
  bool IsInitialized() const { return id_ != 0; }
  int64_t version() const {
    return IsInitialized() ? metadata_.version
                           : IndexedDbDatabaseMetadata::kNoVersion;
  }
  IndexedDbDatabaseMetadata& metadata() { return metadata_; }
  IndexedDbBucket& bucket() { return *bucket_; }
  IndexedDbLockManager& lock_manager();
  const std::list<raw_ptr<IndexedDbConnection>>& connections() const {
    return connections_;
  }
  base::WeakPtr<IndexedDbDatabase> AsWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  void ScheduleOpenConnection(
      std::unique_ptr<IndexedDbFactoryClient> client,
      std::unique_ptr<IndexedDbDatabaseCallbacks> callbacks,
      uintptr_t client_token,
      int64_t version,
      uint64_t transaction_id);
  void ScheduleDeleteDatabase(std::unique_ptr<IndexedDbFactoryClient> client);
  // Runs the open and delete requests and the transactions' tasks. Returns
  // false after an error, which closes the bucket.
  bool RunTasks();
  bool CanBeDestroyed() const;

  std::unique_ptr<IndexedDbConnection> CreateConnection(
      std::unique_ptr<IndexedDbDatabaseCallbacks> callbacks,
      uintptr_t client_token);
  // `forward_on_close` runs unless every connection is closing.
  void ConnectionClosed(IndexedDbConnection& connection,
                        base::OnceClosure forward_on_close);
  void VersionChangeIgnored();
  void RegisterAndScheduleTransaction(IndexedDbTransaction& transaction);
  // BuildLockRequestsForLevelDb().
  std::vector<IndexedDbLockManager::LockRequest> BuildLockRequests(
      IndexedDbTransactionMode mode,
      const std::set<int64_t>& scope) const;
  bool OnlyHasOneClient() const;
  void ForceCloseConnectionsAndCancelRequests(const std::string& message);

  // Loads the database, creating it without a version if there is none.
  bool OpenInternal();
  // Deletes it, and returns its old version, or nullopt after an error.
  std::optional<int64_t> DeleteDatabase();

  // The operations that a transaction runs. Each answers its call, and
  // returns false after an error, or for a call that no renderer makes.
  bool VersionChangeOperation(int64_t version, IndexedDbTransaction& txn);
  bool CreateObjectStoreOperation(int64_t object_store_id,
                                  std::u16string name,
                                  IDBKeyPath key_path,
                                  bool auto_increment,
                                  IndexedDbTransaction& txn);
  bool DeleteObjectStoreOperation(int64_t object_store_id,
                                  IndexedDbTransaction& txn);
  bool RenameObjectStoreOperation(int64_t object_store_id,
                                  std::u16string new_name,
                                  IndexedDbTransaction& txn);
  bool CreateIndexOperation(int64_t object_store_id,
                            IndexedDbIndexMetadata index,
                            IndexedDbTransaction& txn);
  bool DeleteIndexOperation(int64_t object_store_id,
                            int64_t index_id,
                            IndexedDbTransaction& txn);
  bool RenameIndexOperation(int64_t object_store_id,
                            int64_t index_id,
                            std::u16string new_name,
                            IndexedDbTransaction& txn);
  bool GetOperation(int64_t object_store_id,
                    int64_t index_id,
                    IndexedDbKeyRange key_range,
                    bool key_only,
                    IndexedDbReply reply,
                    IndexedDbTransaction& txn);
  bool GetAllOperation(int64_t object_store_id,
                       int64_t index_id,
                       IndexedDbKeyRange key_range,
                       IndexedDbGetAllResultType result_type,
                       uint32_t max_count,
                       IndexedDbCursorDirection direction,
                       std::unique_ptr<IndexedDbGetAllSink> sink,
                       IndexedDbTransaction& txn);
  bool OpenCursorOperation(
      int64_t object_store_id,
      int64_t index_id,
      IndexedDbKeyRange key_range,
      IndexedDbCursorDirection direction,
      bool key_only,
      IndexedDbTaskType task_type,
      base::OnceCallback<void(std::unique_ptr<IndexedDbCursor>)> adopt_cursor,
      IndexedDbReply reply,
      IndexedDbTransaction& txn);
  bool CountOperation(int64_t object_store_id,
                      int64_t index_id,
                      IndexedDbKeyRange key_range,
                      IndexedDbReply reply,
                      IndexedDbTransaction& txn);
  bool DeleteRangeOperation(int64_t object_store_id,
                            IndexedDbKeyRange key_range,
                            IndexedDbReply reply,
                            IndexedDbTransaction& txn);
  bool ClearOperation(int64_t object_store_id,
                      IndexedDbReply reply,
                      IndexedDbTransaction& txn);
  bool GetKeyGeneratorCurrentNumberOperation(int64_t object_store_id,
                                             IndexedDbReply reply,
                                             IndexedDbTransaction& txn);

  const IndexedDbObjectStoreMetadata* GetObjectStoreMetadata(
      int64_t object_store_id) const;
  // Whether the object store is there, and the index, unless `index_id` is
  // kInvalidId.
  bool IsObjectStoreAndMaybeIndexInMetadata(int64_t object_store_id,
                                            int64_t index_id) const;
  void SendVersionChangeToAllConnections(int64_t old_version,
                                         int64_t new_version);

 private:
  // The connection coordinator's: an open or delete request, which waits for
  // the connections to close and for the database's lock.
  class Request;
  class OpenRequest;
  class DeleteRequest;
  enum class RequestResult {
    kDone,
    kPendingAsyncWork,
    kMoreTasks,
    kError,
  };

  RequestResult ExecuteRequest(bool has_connections);
  void CancelPendingRequests(const std::string& message);
  void OnNoConnections();
  void OnUpgradeTransactionStarted(int64_t old_version);
  void OnUpgradeTransactionFinished(bool committed);

  const std::u16string name_;
  const raw_ref<IndexedDbBucket> bucket_;
  // The number of the database in the backing store: 0 until it is loaded.
  int64_t id_ = 0;
  IndexedDbDatabaseMetadata metadata_;
  std::list<raw_ptr<IndexedDbConnection>> connections_;
  // The open and delete requests, which run one at a time, in order.
  base::queue<std::unique_ptr<Request>> requests_;
  bool closing_all_connections_ = false;
  base::WeakPtrFactory<IndexedDbDatabase> weak_factory_{this};
};

// The IndexedDB of one storage key: its backing store, its locks, and its
// databases, which live while they have connections or requests.
class IndexedDbBucket {
 public:
  IndexedDbBucket(std::unique_ptr<IndexedDbBackingStore> backing_store,
                  std::unique_ptr<IndexedDbQuota> quota);
  IndexedDbBucket(const IndexedDbBucket&) = delete;
  IndexedDbBucket& operator=(const IndexedDbBucket&) = delete;
  ~IndexedDbBucket();

  IndexedDbBackingStore& backing_store() { return *backing_store_; }
  IndexedDbLockManager& lock_manager() { return lock_manager_; }
  IndexedDbQuota& quota() { return *quota_; }
  base::WeakPtr<IndexedDbBucket> AsWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  // IDBFactory.
  void GetDatabaseInfo(IndexedDbReply reply);
  void Open(std::u16string name,
            int64_t version,
            uint64_t transaction_id,
            uintptr_t client_token,
            std::unique_ptr<IndexedDbFactoryClient> client,
            std::unique_ptr<IndexedDbDatabaseCallbacks> callbacks);
  void DeleteDatabase(std::u16string name,
                      std::unique_ptr<IndexedDbFactoryClient> client);

  // Runs every database's tasks from a task of its own.
  void QueueRunTasks();
  // Reports what the backing store holds now to the quota.
  void UpdateUsage();
  // Closes every connection and cancels every request, with `message`, as
  // at the end of the Session or after an error.
  void ForceClose(const std::string& message);

 private:
  void RunTasks();
  IndexedDbDatabase& GetOrCreateDatabase(const std::u16string& name);

  std::unique_ptr<IndexedDbBackingStore> backing_store_;
  std::unique_ptr<IndexedDbQuota> quota_;
  IndexedDbLockManager lock_manager_;
  std::map<std::u16string, std::unique_ptr<IndexedDbDatabase>> databases_;
  bool task_run_queued_ = false;
  int64_t reported_size_ = 0;
  base::WeakPtrFactory<IndexedDbBucket> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_INDEXED_DB_ENGINE_H_
