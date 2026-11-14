#include <algorithm>
#include <array>
#include <utility>

#include "base/auto_reset.h"
#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/notreached.h"
#include "base/numerics/byte_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "mitto/indexed_db/engine.h"

namespace mitto {
namespace {

// blink.mojom.kIDBGetAllChunkSize: the records of one getAll() answer.
constexpr size_t kGetAllChunkSize = 1000;
// What the records of one answer hold at most. Chrome's values are at most
// 64 KiB, for it wraps bigger ones into blobs; the values here are whole, so
// a chunk also ends before it would outgrow a frame.
constexpr size_t kMaxGetAllChunkBytes = 8 * 1024 * 1024;

// The index IDs below Chrome's on-disk store's first, which an object store
// starts from.
constexpr int64_t kMinimumIndexId = 30;

constexpr int kDatabaseLockPartition = 0;
constexpr int kObjectStoreLockPartition = 1;

// GetObjectStoreLockIdKey(): the database's number and the object store's
// ID, big-endian.
std::string ObjectStoreLockKey(int64_t database_id, int64_t object_store_id) {
  std::array<uint8_t, 16> bytes;
  auto [database, object_store] = base::span(bytes).split_at<8>();
  database.copy_from(base::U64ToBigEndian(static_cast<uint64_t>(database_id)));
  object_store.copy_from(
      base::U64ToBigEndian(static_cast<uint64_t>(object_store_id)));
  return std::string(bytes.begin(), bytes.end());
}

// ConvertValueToReturnValue(): a key that a key generator gave goes with the
// value, for the Page to put into it.
IndexedDbReturnValue ReturnValue(const IndexedDbObjectStoreMetadata& store,
                                 std::string bits,
                                 const IDBKey& primary_key) {
  IndexedDbReturnValue value;
  value.bits = std::move(bits);
  if (store.auto_increment && !store.key_path.IsNull()) {
    value.primary_key = primary_key;
    value.key_path = store.key_path;
  }
  return value;
}

size_t RecordSize(const IndexedDbRecord& record) {
  size_t size = 0;
  if (record.primary_key) {
    size += record.primary_key->SizeEstimate();
  }
  if (record.value) {
    size +=
        record.value->bits.size() + record.value->primary_key.SizeEstimate();
  }
  if (record.index_key) {
    size += record.index_key->SizeEstimate();
  }
  return size;
}

// Transaction::ObjectStoreMustExist().
bool VerifyObjectStore(IndexedDbDatabase& database,
                       IndexedDbTransaction& transaction,
                       int64_t object_store_id) {
  if (database.GetObjectStoreMetadata(object_store_id)) {
    return true;
  }
  transaction.connection().ReportBadMessage("Invalid object_store_id");
  return false;
}

// Transaction::ObjectStoreAndIndexMustExist().
bool VerifyObjectStoreAndIndex(IndexedDbDatabase& database,
                               IndexedDbTransaction& transaction,
                               int64_t object_store_id,
                               int64_t index_id,
                               bool index_is_optional) {
  if (!index_is_optional && index_id == IndexedDbIndexMetadata::kInvalidId) {
    transaction.connection().ReportBadMessage("index_id must be valid");
    return false;
  }
  if (database.IsObjectStoreAndMaybeIndexInMetadata(object_store_id,
                                                    index_id)) {
    return true;
  }
  transaction.connection().ReportBadMessage(
      "Invalid object_store_id or index_id");
  return false;
}

}  // namespace

IndexedDbGetAllSink::IndexedDbGetAllSink(
    base::RepeatingCallback<void(PageIndexedDbCallback)> sink,
    base::WeakPtr<IndexedDbTransaction> transaction)
    : sink_(std::move(sink)), transaction_(std::move(transaction)) {}

IndexedDbGetAllSink::~IndexedDbGetAllSink() {
  if (done_) {
    return;
  }
  if (transaction_) {
    transaction_->IncrementNumErrorsSent();
    SendError(IndexedDbException::kIgnorableAbortError,
              u"Backend aborted error");
  } else {
    SendResults({}, /*done=*/true);
  }
}

void IndexedDbGetAllSink::SendResults(std::vector<IndexedDbRecord> records,
                                      bool done) {
  done_ = done;
  PageIndexedDbCallback answer;
  answer.op = IndexedDbCallbackOp::kGetAllResult;
  answer.records = std::move(records);
  answer.done = done;
  sink_.Run(std::move(answer));
}

void IndexedDbGetAllSink::SendError(IndexedDbException code,
                                    std::u16string message) {
  done_ = true;
  PageIndexedDbCallback answer;
  answer.op = IndexedDbCallbackOp::kGetAllResult;
  answer.error = code;
  answer.message = std::move(message);
  answer.done = true;
  sink_.Run(std::move(answer));
}

// ConnectionCoordinator::ConnectionRequest: what script calls an
// IDBOpenDBRequest, an open or a delete, which may wait for the other
// connections to close.
class IndexedDbDatabase::Request {
 public:
  enum class State {
    kNotStarted,
    kPendingNoConnections,
    kPendingLocks,
    kPendingTransactionComplete,
    kDone,
    kError,
  };

  Request(IndexedDbDatabase& database,
          std::unique_ptr<IndexedDbFactoryClient> client)
      : database_(database), client_(std::move(client)) {}
  Request(const Request&) = delete;
  Request& operator=(const Request&) = delete;
  virtual ~Request() = default;

  // The request is first in the queue. Its state is read after the call.
  virtual void Perform(bool has_connections) = 0;
  // A connection ignored "versionchange": the request is blocked.
  void OnVersionChangeIgnored() {
    if (!blocked_called_ && client_) {
      blocked_called_ = true;
      client_->Blocked(database_->version());
    }
  }
  virtual void OnNoConnections() = 0;
  virtual void UpgradeTransactionStarted(int64_t old_version) = 0;
  virtual void UpgradeTransactionFinished(bool committed) = 0;
  // Ends the request, which the database drops after.
  virtual void OnForceClose(const std::string& message) = 0;

  State state() const { return state_; }

 protected:
  std::unique_ptr<IndexedDbFactoryClient> TakeClient() {
    return std::move(client_);
  }

  void ContinueAfterAcquiringLocks(base::OnceClosure next_step) {
    if (!locks_.locks.empty()) {
      std::move(next_step).Run();
      return;
    }
    state_ = State::kPendingLocks;
    database_->lock_manager().AcquireLocks(
        database_->BuildLockRequests(IndexedDbTransactionMode::kVersionChange,
                                     {}),
        locks_, std::move(next_step));
  }

  // What has the bucket run its tasks, unless the request is being
  // performed, after which they are run anyway.
  base::OnceClosure TasksAvailable() {
    if (performing_) {
      return base::DoNothing();
    }
    return base::BindOnce(&IndexedDbBucket::QueueRunTasks,
                          database_->bucket().AsWeakPtr());
  }

  State state_ = State::kNotStarted;
  const raw_ref<IndexedDbDatabase> database_;
  std::unique_ptr<IndexedDbFactoryClient> client_;
  bool blocked_called_ = false;
  bool performing_ = false;
  IndexedDbLockHolder locks_;
};

class IndexedDbDatabase::OpenRequest : public IndexedDbDatabase::Request {
 public:
  OpenRequest(IndexedDbDatabase& database,
              std::unique_ptr<IndexedDbFactoryClient> client,
              std::unique_ptr<IndexedDbDatabaseCallbacks> callbacks,
              uintptr_t client_token,
              int64_t version,
              uint64_t transaction_id)
      : Request(database, std::move(client)),
        callbacks_(std::move(callbacks)),
        client_token_(client_token),
        version_(version),
        transaction_id_(transaction_id) {}

  void Perform(bool has_connections) override {
    base::AutoReset<bool> performing(&performing_, true);
    // The metadata is read under the database's lock, so that it is not
    // stale.
    if (!database_->IsInitialized()) {
      ContinueAfterAcquiringLocks(base::BindOnce(&OpenRequest::InitDatabase,
                                                 weak_factory_.GetWeakPtr(),
                                                 has_connections));
      return;
    }
    ContinueOpening(has_connections);
  }

  void OnNoConnections() override {
    ContinueAfterAcquiringLocks(
        base::BindOnce(&OpenRequest::StartUpgrade, weak_factory_.GetWeakPtr()));
  }

  void UpgradeTransactionStarted(int64_t old_version) override {
    CHECK(state_ == State::kPendingTransactionComplete);
    CHECK(upgrade_connection_);
    if (!client_ || !client_->IsConnected()) {
      // Not while the transaction's tasks run.
      base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
          FROM_HERE, std::move(upgrade_connection_));
      return;
    }
    client_->UpgradeNeeded(std::move(upgrade_connection_), old_version,
                           database_->metadata());
  }

  void UpgradeTransactionFinished(bool committed) override {
    CHECK(state_ == State::kPendingTransactionComplete);
    // The client has the connection since UpgradeNeeded.
    if (committed) {
      CHECK_EQ(version_, database_->version());
      OnOpenSuccess(nullptr);
    } else if (client_) {
      TakeClient()->Error(IndexedDbException::kAbortError,
                          u"Version change transaction was aborted in "
                          u"upgradeneeded event handler.");
    }
    state_ = State::kDone;
    TasksAvailable().Run();
  }

  void OnForceClose(const std::string& message) override {
    if (client_) {
      TakeClient()->Error(IndexedDbException::kAbortError,
                          u"The connection was closed.");
    }
    if (upgrade_connection_) {
      // This tells the connection's callbacks.
      upgrade_connection_->CloseAndReportForceClose(message);
    } else if (callbacks_) {
      callbacks_->ForcedClose();
    }
  }

 private:
  void InitDatabase(bool has_connections) {
    if (!database_->OpenInternal()) {
      std::u16string message =
          version_ == IndexedDbDatabaseMetadata::kNoVersion
              ? u"Internal error opening database with no version specified."
              : base::StrCat({u"Internal error opening database with version ",
                              base::NumberToString16(version_)});
      TakeClient()->Error(IndexedDbException::kUnknownError,
                          std::move(message));
      state_ = State::kError;
      TasksAvailable().Run();
      return;
    }
    ContinueOpening(has_connections);
  }

  void ContinueOpening(bool has_connections) {
    base::ScopedClosureRunner tasks_available(TasksAvailable());
    const int64_t old_version = database_->version();
    CHECK(version_ >= 1 || version_ == IndexedDbDatabaseMetadata::kNoVersion);
    const bool is_new_database =
        old_version == IndexedDbDatabaseMetadata::kNoVersion;
    if (!is_new_database &&
        (version_ == old_version ||
         version_ == IndexedDbDatabaseMetadata::kNoVersion)) {
      OnOpenSuccess(
          database_->CreateConnection(std::move(callbacks_), client_token_));
      state_ = State::kDone;
      return;
    }
    if (version_ == IndexedDbDatabaseMetadata::kNoVersion) {
      // A new database without a version gets version 1.
      CHECK(is_new_database);
      version_ = 1;
    } else if (version_ < old_version) {
      CHECK(!is_new_database);
      TakeClient()->Error(
          IndexedDbException::kVersionError,
          base::StrCat({u"The requested version (",
                        base::NumberToString16(version_),
                        u") is less than the existing version (",
                        base::NumberToString16(old_version), u")."}));
      state_ = State::kDone;
      return;
    }
    CHECK_GT(version_, old_version);
    if (!has_connections) {
      OnNoConnections();
      return;
    }
    // The other connections hear "versionchange"; the upgrade waits until
    // they close, and is blocked when one ignores it.
    state_ = State::kPendingNoConnections;
    database_->SendVersionChangeToAllConnections(old_version, version_);
  }

  void OnConnectionClosedDuringUpgrade() {
    if (client_) {
      TakeClient()->Error(IndexedDbException::kAbortError,
                          u"The connection was closed.");
    }
    state_ = State::kDone;
    TasksAvailable().Run();
  }

  // `connection` is null after an upgrade, whose connection the client got
  // with UpgradeNeeded.
  void OnOpenSuccess(std::unique_ptr<IndexedDbConnection> connection) {
    if (!client_ || !client_->IsConnected()) {
      if (connection) {
        base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
            FROM_HERE, std::move(connection));
      }
      return;
    }
    TakeClient()->OpenSuccess(std::move(connection), database_->metadata());
  }

  // The upgrade itself happens in VersionChangeOperation(), which starts the
  // transaction.
  void StartUpgrade() {
    CHECK(state_ == State::kPendingLocks);
    CHECK(!locks_.locks.empty());
    upgrade_connection_ =
        database_->CreateConnection(std::move(callbacks_), client_token_);
    upgrade_connection_->SetOnCloseDuringUpgrade(
        base::BindOnce(&OpenRequest::OnConnectionClosedDuringUpgrade,
                       weak_factory_.GetWeakPtr()));
    state_ = State::kPendingTransactionComplete;
    IndexedDbTransaction* const transaction =
        upgrade_connection_->CreateVersionChangeTransaction(transaction_id_);
    transaction->ScheduleTask(
        BindWeakOperation(&IndexedDbDatabase::VersionChangeOperation,
                          database_->AsWeakPtr(), version_));
    transaction->locks_holder().locks = std::move(locks_.locks);
    transaction->Start();
  }

  std::unique_ptr<IndexedDbDatabaseCallbacks> callbacks_;
  const uintptr_t client_token_;
  int64_t version_;
  const uint64_t transaction_id_;
  // The connection of an upgrade until the client has it.
  std::unique_ptr<IndexedDbConnection> upgrade_connection_;
  base::WeakPtrFactory<OpenRequest> weak_factory_{this};
};

class IndexedDbDatabase::DeleteRequest : public IndexedDbDatabase::Request {
 public:
  using Request::Request;

  void Perform(bool has_connections) override {
    base::AutoReset<bool> performing(&performing_, true);
    if (!database_->bucket().backing_store().LoadDatabase(database_->name())) {
      // The old version of a database that is not there is 0.
      TakeClient()->DeleteSuccess(0);
      state_ = State::kDone;
      return;
    }
    if (!database_->IsInitialized()) {
      ContinueAfterAcquiringLocks(base::BindOnce(&DeleteRequest::InitDatabase,
                                                 weak_factory_.GetWeakPtr(),
                                                 has_connections));
      return;
    }
    ContinueDeleting(has_connections);
  }

  void OnNoConnections() override {
    ContinueAfterAcquiringLocks(
        base::BindOnce(&DeleteRequest::DoDelete, weak_factory_.GetWeakPtr()));
  }

  void UpgradeTransactionStarted(int64_t old_version) override { NOTREACHED(); }

  void UpgradeTransactionFinished(bool committed) override { NOTREACHED(); }

  void OnForceClose(const std::string& message) override {
    if (client_) {
      TakeClient()->Error(IndexedDbException::kAbortError,
                          u"The connection was closed.");
    }
  }

 private:
  void InitDatabase(bool has_connections) {
    base::ScopedClosureRunner tasks_available(TasksAvailable());
    if (!database_->OpenInternal()) {
      TakeClient()->Error(IndexedDbException::kUnknownError,
                          u"Internal error creating database backend for "
                          u"indexedDB.deleteDatabase.");
      state_ = State::kError;
      return;
    }
    ContinueDeleting(has_connections);
  }

  void ContinueDeleting(bool has_connections) {
    if (!has_connections) {
      OnNoConnections();
      return;
    }
    state_ = State::kPendingNoConnections;
    database_->SendVersionChangeToAllConnections(
        database_->version(), IndexedDbDatabaseMetadata::kNoVersion);
  }

  void DoDelete() {
    state_ = State::kPendingTransactionComplete;
    const std::optional<int64_t> old_version = database_->DeleteDatabase();
    base::ScopedClosureRunner tasks_available(TasksAvailable());
    if (!old_version) {
      TakeClient()->Error(IndexedDbException::kUnknownError,
                          u"Internal error deleting database.");
      state_ = State::kError;
      return;
    }
    // A database without a version was never there.
    TakeClient()->DeleteSuccess(*old_version ==
                                        IndexedDbDatabaseMetadata::kNoVersion
                                    ? IndexedDbDatabaseMetadata::kDefaultVersion
                                    : *old_version);
    state_ = State::kDone;
  }

  base::WeakPtrFactory<DeleteRequest> weak_factory_{this};
};

IndexedDbDatabase::IndexedDbDatabase(std::u16string name,
                                     IndexedDbBucket& bucket)
    : name_(std::move(name)), bucket_(bucket) {
  metadata_.name = name_;
}

IndexedDbDatabase::~IndexedDbDatabase() = default;

IndexedDbLockManager& IndexedDbDatabase::lock_manager() {
  return bucket_->lock_manager();
}

void IndexedDbDatabase::ScheduleOpenConnection(
    std::unique_ptr<IndexedDbFactoryClient> client,
    std::unique_ptr<IndexedDbDatabaseCallbacks> callbacks,
    uintptr_t client_token,
    int64_t version,
    uint64_t transaction_id) {
  requests_.push(std::make_unique<OpenRequest>(
      *this, std::move(client), std::move(callbacks), client_token, version,
      transaction_id));
  bucket_->QueueRunTasks();
}

void IndexedDbDatabase::ScheduleDeleteDatabase(
    std::unique_ptr<IndexedDbFactoryClient> client) {
  requests_.push(std::make_unique<DeleteRequest>(*this, std::move(client)));
  bucket_->QueueRunTasks();
}

IndexedDbDatabase::RequestResult IndexedDbDatabase::ExecuteRequest(
    bool has_connections) {
  if (requests_.empty()) {
    return RequestResult::kDone;
  }
  Request& request = *requests_.front();
  if (request.state() == Request::State::kNotStarted) {
    request.Perform(has_connections);
    CHECK(request.state() != Request::State::kNotStarted);
  }
  switch (request.state()) {
    case Request::State::kNotStarted:
      NOTREACHED();
    case Request::State::kPendingNoConnections:
    case Request::State::kPendingLocks:
    case Request::State::kPendingTransactionComplete:
      return RequestResult::kPendingAsyncWork;
    case Request::State::kDone:
    case Request::State::kError: {
      // Out of the queue first: dropping it may close its connection, which
      // looks at the queue.
      const bool error = request.state() == Request::State::kError;
      std::unique_ptr<Request> discarded = std::move(requests_.front());
      requests_.pop();
      discarded.reset();
      if (error) {
        return RequestResult::kError;
      }
      return requests_.empty() ? RequestResult::kDone
                               : RequestResult::kMoreTasks;
    }
  }
  NOTREACHED();
}

void IndexedDbDatabase::CancelPendingRequests(const std::string& message) {
  while (!requests_.empty()) {
    std::unique_ptr<Request> request = std::move(requests_.front());
    requests_.pop();
    request->OnForceClose(message);
  }
}

void IndexedDbDatabase::OnNoConnections() {
  if (requests_.empty() ||
      requests_.front()->state() != Request::State::kPendingNoConnections) {
    return;
  }
  requests_.front()->OnNoConnections();
}

void IndexedDbDatabase::VersionChangeIgnored() {
  if (requests_.empty() ||
      requests_.front()->state() != Request::State::kPendingNoConnections) {
    return;
  }
  requests_.front()->OnVersionChangeIgnored();
}

void IndexedDbDatabase::OnUpgradeTransactionStarted(int64_t old_version) {
  if (requests_.empty() || requests_.front()->state() !=
                               Request::State::kPendingTransactionComplete) {
    return;
  }
  requests_.front()->UpgradeTransactionStarted(old_version);
}

void IndexedDbDatabase::OnUpgradeTransactionFinished(bool committed) {
  if (requests_.empty() || requests_.front()->state() !=
                               Request::State::kPendingTransactionComplete) {
    return;
  }
  requests_.front()->UpgradeTransactionFinished(committed);
}

bool IndexedDbDatabase::RunTasks() {
  while (true) {
    const RequestResult result = ExecuteRequest(!connections_.empty());
    if (result == RequestResult::kError) {
      return false;
    }
    if (result != RequestResult::kMoreTasks) {
      break;
    }
  }
  // Runs the transactions that have tasks, and drops those that finished.
  bool transactions_removed = true;
  while (transactions_removed) {
    transactions_removed = false;
    for (IndexedDbConnection* connection : connections_) {
      std::optional<bool> upgrade_committed;
      std::vector<uint64_t> finished;
      for (const auto& [id, transaction] : connection->transactions()) {
        switch (transaction->state()) {
          case IndexedDbTransaction::kCreated:
            continue;
          case IndexedDbTransaction::kStarted:
          case IndexedDbTransaction::kCommitting:
            if (!transaction->RunTasks()) {
              return false;
            }
            break;
          case IndexedDbTransaction::kFinished:
            break;
        }
        if (transaction->state() == IndexedDbTransaction::kFinished) {
          if (transaction->mode() == IndexedDbTransactionMode::kVersionChange) {
            upgrade_committed = !transaction->aborted();
          }
          finished.push_back(id);
        }
      }
      for (uint64_t id : finished) {
        connection->RemoveTransaction(id);
        transactions_removed = true;
      }
      if (upgrade_committed) {
        OnUpgradeTransactionFinished(*upgrade_committed);
      }
    }
  }
  return true;
}

bool IndexedDbDatabase::CanBeDestroyed() const {
  return requests_.empty() && connections_.empty();
}

std::unique_ptr<IndexedDbConnection> IndexedDbDatabase::CreateConnection(
    std::unique_ptr<IndexedDbDatabaseCallbacks> callbacks,
    uintptr_t client_token) {
  auto connection = std::make_unique<IndexedDbConnection>(
      *this, std::move(callbacks), client_token);
  connections_.push_back(connection.get());
  return connection;
}

void IndexedDbDatabase::ConnectionClosed(IndexedDbConnection& connection,
                                         base::OnceClosure forward_on_close) {
  if (closing_all_connections_) {
    return;
  }
  const size_t removed = std::erase(connections_, &connection);
  CHECK_EQ(removed, 1u);
  if (forward_on_close) {
    std::move(forward_on_close).Run();
  }
  if (connections_.empty()) {
    OnNoConnections();
  }
  if (CanBeDestroyed()) {
    bucket_->QueueRunTasks();
  }
}

void IndexedDbDatabase::RegisterAndScheduleTransaction(
    IndexedDbTransaction& transaction) {
  // An upgrade's locks are its request's.
  CHECK_NE(transaction.mode(), IndexedDbTransactionMode::kVersionChange);
  lock_manager().AcquireLocks(
      BuildLockRequests(transaction.mode(), transaction.scope()),
      transaction.locks_holder(),
      base::BindOnce(&IndexedDbTransaction::Start, transaction.AsWeakPtr()));
}

std::vector<IndexedDbLockManager::LockRequest>
IndexedDbDatabase::BuildLockRequests(IndexedDbTransactionMode mode,
                                     const std::set<int64_t>& scope) const {
  IndexedDbLockId database_lock{kDatabaseLockPartition,
                                base::UTF16ToUTF8(name_)};
  if (mode == IndexedDbTransactionMode::kVersionChange) {
    return {
        {std::move(database_lock), IndexedDbLockManager::LockType::kExclusive}};
  }
  std::vector<IndexedDbLockManager::LockRequest> requests;
  requests.reserve(1 + scope.size());
  requests.push_back(
      {std::move(database_lock), IndexedDbLockManager::LockType::kShared});
  const IndexedDbLockManager::LockType object_store_lock_type =
      mode == IndexedDbTransactionMode::kReadOnly
          ? IndexedDbLockManager::LockType::kShared
          : IndexedDbLockManager::LockType::kExclusive;
  for (int64_t object_store_id : scope) {
    requests.push_back(
        {{kObjectStoreLockPartition, ObjectStoreLockKey(id_, object_store_id)},
         object_store_lock_type});
  }
  return requests;
}

bool IndexedDbDatabase::OnlyHasOneClient() const {
  if (connections_.empty()) {
    return true;
  }
  const uintptr_t token = connections_.front()->client_token();
  return std::ranges::all_of(connections_,
                             [token](const IndexedDbConnection* connection) {
                               return connection->client_token() == token;
                             });
}

void IndexedDbDatabase::ForceCloseConnectionsAndCancelRequests(
    const std::string& message) {
  base::AutoReset<bool> closing(&closing_all_connections_, true);
  for (IndexedDbConnection* connection : connections_) {
    connection->CloseAndReportForceClose(message);
  }
  connections_.clear();
  CancelPendingRequests(message);
}

bool IndexedDbDatabase::OpenInternal() {
  IndexedDbBackingStore& store = bucket_->backing_store();
  if (std::optional<std::pair<int64_t, IndexedDbDatabaseMetadata>> loaded =
          store.LoadDatabase(name_)) {
    id_ = loaded->first;
    metadata_ = std::move(loaded->second);
    metadata_.name = name_;
    return true;
  }
  const std::optional<int64_t> id = store.CreateDatabase(name_);
  if (!id) {
    return false;
  }
  id_ = *id;
  metadata_ = IndexedDbDatabaseMetadata();
  metadata_.name = name_;
  bucket_->UpdateUsage();
  return true;
}

std::optional<int64_t> IndexedDbDatabase::DeleteDatabase() {
  if (!IsInitialized()) {
    return IndexedDbDatabaseMetadata::kDefaultVersion;
  }
  const int64_t old_version = version();
  const bool deleted = bucket_->backing_store().DeleteDatabase(id_, name_);
  id_ = 0;
  metadata_ = IndexedDbDatabaseMetadata();
  metadata_.name = name_;
  if (!deleted) {
    return std::nullopt;
  }
  bucket_->UpdateUsage();
  return old_version;
}

bool IndexedDbDatabase::VersionChangeOperation(int64_t version,
                                               IndexedDbTransaction& txn) {
  const int64_t old_version = metadata_.version;
  CHECK_GT(version, old_version);
  if (!txn.backing_store().SetDatabaseVersion(version,
                                              metadata_.max_object_store_id)) {
    return false;
  }
  metadata_.version = version;
  OnUpgradeTransactionStarted(old_version);
  return true;
}

bool IndexedDbDatabase::CreateObjectStoreOperation(int64_t object_store_id,
                                                   std::u16string name,
                                                   IDBKeyPath key_path,
                                                   bool auto_increment,
                                                   IndexedDbTransaction& txn) {
  // The ID must be a new one.
  if (metadata_.object_stores.contains(object_store_id) ||
      object_store_id <= metadata_.max_object_store_id) {
    txn.connection().ReportBadMessage("Invalid object_store_id");
    return false;
  }
  IndexedDbObjectStoreMetadata store;
  store.id = object_store_id;
  store.name = std::move(name);
  store.key_path = std::move(key_path);
  store.auto_increment = auto_increment;
  store.max_index_id = kMinimumIndexId;
  if (!txn.backing_store().CreateObjectStore(store) ||
      !txn.backing_store().SetDatabaseVersion(metadata_.version,
                                              object_store_id)) {
    return false;
  }
  metadata_.max_object_store_id = object_store_id;
  metadata_.object_stores.emplace(object_store_id, std::move(store));
  return true;
}

bool IndexedDbDatabase::DeleteObjectStoreOperation(int64_t object_store_id,
                                                   IndexedDbTransaction& txn) {
  if (!VerifyObjectStore(*this, txn, object_store_id)) {
    return false;
  }
  if (!txn.backing_store().DeleteObjectStore(object_store_id)) {
    return false;
  }
  metadata_.object_stores.erase(object_store_id);
  return true;
}

bool IndexedDbDatabase::RenameObjectStoreOperation(int64_t object_store_id,
                                                   std::u16string new_name,
                                                   IndexedDbTransaction& txn) {
  if (!VerifyObjectStore(*this, txn, object_store_id)) {
    return false;
  }
  IndexedDbObjectStoreMetadata& store =
      metadata_.object_stores.at(object_store_id);
  store.name = std::move(new_name);
  return txn.backing_store().UpdateObjectStore(store);
}

bool IndexedDbDatabase::CreateIndexOperation(int64_t object_store_id,
                                             IndexedDbIndexMetadata index,
                                             IndexedDbTransaction& txn) {
  // The object store is there, the index is not, and its ID is a new one.
  const auto store_it = metadata_.object_stores.find(object_store_id);
  if (store_it == metadata_.object_stores.end() ||
      store_it->second.indexes.contains(index.id) ||
      store_it->second.max_index_id >= index.id) {
    txn.connection().ReportBadMessage("Invalid object_store_id or index_id.");
    return false;
  }
  IndexedDbObjectStoreMetadata& store = store_it->second;
  if (!txn.backing_store().CreateIndex(object_store_id, index)) {
    return false;
  }
  store.max_index_id = index.id;
  if (!txn.backing_store().UpdateObjectStore(store)) {
    return false;
  }
  const int64_t index_id = index.id;
  store.indexes.emplace(index_id, std::move(index));
  return true;
}

bool IndexedDbDatabase::DeleteIndexOperation(int64_t object_store_id,
                                             int64_t index_id,
                                             IndexedDbTransaction& txn) {
  if (!VerifyObjectStoreAndIndex(*this, txn, object_store_id, index_id,
                                 /*index_is_optional=*/false)) {
    return false;
  }
  if (!txn.backing_store().DeleteIndex(object_store_id, index_id)) {
    return false;
  }
  metadata_.object_stores.at(object_store_id).indexes.erase(index_id);
  return true;
}

bool IndexedDbDatabase::RenameIndexOperation(int64_t object_store_id,
                                             int64_t index_id,
                                             std::u16string new_name,
                                             IndexedDbTransaction& txn) {
  if (!VerifyObjectStoreAndIndex(*this, txn, object_store_id, index_id,
                                 /*index_is_optional=*/false)) {
    return false;
  }
  IndexedDbIndexMetadata& index =
      metadata_.object_stores.at(object_store_id).indexes.at(index_id);
  index.name = std::move(new_name);
  // Writing the index's metadata again keeps its entries.
  return txn.backing_store().CreateIndex(object_store_id, index);
}

bool IndexedDbDatabase::GetOperation(int64_t object_store_id,
                                     int64_t index_id,
                                     IndexedDbKeyRange key_range,
                                     bool key_only,
                                     IndexedDbReply reply,
                                     IndexedDbTransaction& txn) {
  if (!VerifyObjectStoreAndIndex(*this, txn, object_store_id, index_id,
                                 /*index_is_optional=*/true)) {
    return false;
  }
  const IndexedDbObjectStoreMetadata& store =
      metadata_.object_stores.at(object_store_id);
  PageIndexedDbCallback answer;
  answer.op = IndexedDbCallbackOp::kGetResult;
  const auto empty = [&]() {
    answer.empty = true;
    std::move(reply).Run(std::move(answer));
    return true;
  };
  IndexedDbBackingStore::Transaction& backing_store = txn.backing_store();
  IDBKey key;
  if (key_range.IsOnlyKey()) {
    key = key_range.lower;
  } else {
    std::unique_ptr<IndexedDbBackingStore::Cursor> cursor =
        backing_store.OpenCursor(object_store_id, index_id, key_range,
                                 IndexedDbCursorDirection::kNext,
                                 /*key_only=*/true);
    if (!cursor) {
      return empty();
    }
    key = cursor->key();
  }
  IDBKey primary_key;
  if (index_id == IndexedDbIndexMetadata::kInvalidId) {
    primary_key = std::move(key);
  } else {
    // The first entry of the index key.
    std::unique_ptr<IndexedDbBackingStore::Cursor> cursor =
        backing_store.OpenCursor(object_store_id, index_id,
                                 IndexedDbKeyRange::Only(std::move(key)),
                                 IndexedDbCursorDirection::kNext,
                                 /*key_only=*/true);
    if (!cursor) {
      return empty();
    }
    primary_key = cursor->primary_key();
  }
  std::optional<std::string> value =
      backing_store.GetRecord(object_store_id, primary_key);
  if (!value) {
    return empty();
  }
  if (key_only) {
    answer.key = std::move(primary_key);
  } else {
    answer.value = ReturnValue(store, std::move(*value), primary_key);
  }
  std::move(reply).Run(std::move(answer));
  return true;
}

bool IndexedDbDatabase::GetAllOperation(
    int64_t object_store_id,
    int64_t index_id,
    IndexedDbKeyRange key_range,
    IndexedDbGetAllResultType result_type,
    uint32_t max_count,
    IndexedDbCursorDirection direction,
    std::unique_ptr<IndexedDbGetAllSink> sink,
    IndexedDbTransaction& txn) {
  CHECK_GT(max_count, 0u);
  if (!VerifyObjectStoreAndIndex(*this, txn, object_store_id, index_id,
                                 /*index_is_optional=*/true)) {
    return false;
  }
  const IndexedDbObjectStoreMetadata& store =
      metadata_.object_stores.at(object_store_id);
  std::unique_ptr<IndexedDbBackingStore::Cursor> cursor =
      txn.backing_store().OpenCursor(
          object_store_id, index_id, key_range, direction,
          /*key_only=*/result_type == IndexedDbGetAllResultType::kKeys);
  std::vector<IndexedDbRecord> records;
  if (!cursor) {
    sink->SendResults(std::move(records), /*done=*/true);
    return true;
  }
  size_t records_size = 0;
  for (uint32_t i = 0; i < max_count; ++i) {
    // Opening the cursor found the first.
    if (i != 0 && !cursor->Continue()) {
      break;
    }
    IndexedDbRecord record;
    switch (result_type) {
      case IndexedDbGetAllResultType::kKeys:
        record.primary_key = cursor->primary_key();
        break;
      case IndexedDbGetAllResultType::kValues:
        record.value =
            ReturnValue(store, cursor->value(), cursor->primary_key());
        break;
      case IndexedDbGetAllResultType::kRecords:
        record.primary_key = cursor->primary_key();
        record.value =
            ReturnValue(store, cursor->value(), cursor->primary_key());
        // Only an index's records have an index key.
        if (index_id != IndexedDbIndexMetadata::kInvalidId) {
          record.index_key = cursor->key();
        }
        break;
    }
    const size_t size = RecordSize(record);
    if (!records.empty() && (records.size() >= kGetAllChunkSize ||
                             records_size + size > kMaxGetAllChunkBytes)) {
      sink->SendResults(std::move(records), /*done=*/false);
      records.clear();
      records_size = 0;
    }
    records_size += size;
    records.push_back(std::move(record));
  }
  sink->SendResults(std::move(records), /*done=*/true);
  return true;
}

bool IndexedDbDatabase::OpenCursorOperation(
    int64_t object_store_id,
    int64_t index_id,
    IndexedDbKeyRange key_range,
    IndexedDbCursorDirection direction,
    bool key_only,
    IndexedDbTaskType task_type,
    base::OnceCallback<void(std::unique_ptr<IndexedDbCursor>)> adopt_cursor,
    IndexedDbReply reply,
    IndexedDbTransaction& txn) {
  if (!VerifyObjectStoreAndIndex(*this, txn, object_store_id, index_id,
                                 /*index_is_optional=*/true)) {
    return false;
  }
  // The Page populates an index: the transaction runs only its preemptive
  // tasks until it is done.
  if (task_type == IndexedDbTaskType::kPreemptive) {
    txn.AddPreemptiveEvent();
  }
  PageIndexedDbCallback answer;
  answer.op = IndexedDbCallbackOp::kOpenCursorResult;
  std::unique_ptr<IndexedDbBackingStore::Cursor> backing_store_cursor =
      txn.backing_store().OpenCursor(object_store_id, index_id, key_range,
                                     direction, key_only);
  if (!backing_store_cursor) {
    answer.empty = true;
    std::move(reply).Run(std::move(answer));
    return true;
  }
  auto cursor = std::make_unique<IndexedDbCursor>(
      std::move(backing_store_cursor),
      index_id != IndexedDbIndexMetadata::kInvalidId, direction, key_only,
      task_type, txn.AsWeakPtr());
  PageIndexedDbCallback entry = cursor->CurrentEntry();
  answer.key = std::move(entry.keys.front());
  answer.primary_key = std::move(entry.primary_keys.front());
  if (!entry.values.empty()) {
    answer.value = std::move(entry.values.front());
  }
  std::move(adopt_cursor).Run(std::move(cursor));
  std::move(reply).Run(std::move(answer));
  return true;
}

bool IndexedDbDatabase::CountOperation(int64_t object_store_id,
                                       int64_t index_id,
                                       IndexedDbKeyRange key_range,
                                       IndexedDbReply reply,
                                       IndexedDbTransaction& txn) {
  if (!VerifyObjectStoreAndIndex(*this, txn, object_store_id, index_id,
                                 /*index_is_optional=*/true)) {
    return false;
  }
  PageIndexedDbCallback answer;
  answer.op = IndexedDbCallbackOp::kCountResult;
  answer.success = true;
  answer.count =
      txn.backing_store().Count(object_store_id, index_id, key_range);
  std::move(reply).Run(std::move(answer));
  return true;
}

bool IndexedDbDatabase::DeleteRangeOperation(int64_t object_store_id,
                                             IndexedDbKeyRange key_range,
                                             IndexedDbReply reply,
                                             IndexedDbTransaction& txn) {
  if (!VerifyObjectStore(*this, txn, object_store_id)) {
    return false;
  }
  txn.backing_store().DeleteRange(object_store_id, key_range);
  PageIndexedDbCallback answer;
  answer.op = IndexedDbCallbackOp::kSuccessResult;
  answer.success = true;
  std::move(reply).Run(std::move(answer));
  return true;
}

bool IndexedDbDatabase::ClearOperation(int64_t object_store_id,
                                       IndexedDbReply reply,
                                       IndexedDbTransaction& txn) {
  if (!VerifyObjectStore(*this, txn, object_store_id)) {
    return false;
  }
  txn.backing_store().ClearObjectStore(object_store_id);
  PageIndexedDbCallback answer;
  answer.op = IndexedDbCallbackOp::kSuccessResult;
  answer.success = true;
  std::move(reply).Run(std::move(answer));
  return true;
}

bool IndexedDbDatabase::GetKeyGeneratorCurrentNumberOperation(
    int64_t object_store_id,
    IndexedDbReply reply,
    IndexedDbTransaction& txn) {
  if (!VerifyObjectStore(*this, txn, object_store_id)) {
    return false;
  }
  PageIndexedDbCallback answer;
  answer.op = IndexedDbCallbackOp::kKeyGeneratorResult;
  answer.number =
      txn.backing_store().GetKeyGeneratorCurrentNumber(object_store_id);
  std::move(reply).Run(std::move(answer));
  return true;
}

const IndexedDbObjectStoreMetadata* IndexedDbDatabase::GetObjectStoreMetadata(
    int64_t object_store_id) const {
  const auto found = metadata_.object_stores.find(object_store_id);
  return found == metadata_.object_stores.end() ? nullptr : &found->second;
}

bool IndexedDbDatabase::IsObjectStoreAndMaybeIndexInMetadata(
    int64_t object_store_id,
    int64_t index_id) const {
  const IndexedDbObjectStoreMetadata* const store =
      GetObjectStoreMetadata(object_store_id);
  if (!store) {
    return false;
  }
  return index_id == IndexedDbIndexMetadata::kInvalidId ||
         store->indexes.contains(index_id);
}

void IndexedDbDatabase::SendVersionChangeToAllConnections(int64_t old_version,
                                                          int64_t new_version) {
  // Chrome asks the browser's UI thread first whether each client is active,
  // and tells the connection from the answer's task, so that what the
  // database sends meanwhile, such as the completion of a transaction that
  // keeps a closing connection open, reaches the client first. A Page is
  // never kept inactive, as a frame in the back/forward cache is: the answer
  // is always yes.
  for (IndexedDbConnection* connection : connections_) {
    // A round trip through another thread, as Chrome's is through its UI
    // thread, during which the library reads what the Page sent meanwhile.
    base::ThreadPool::PostTaskAndReply(
        FROM_HERE, base::DoNothing(),
        base::BindOnce(
            [](base::WeakPtr<IndexedDbConnection> connection,
               int64_t old_version, int64_t new_version) {
              if (connection && connection->IsConnected()) {
                connection->callbacks()->VersionChange(old_version,
                                                       new_version);
              }
            },
            connection->AsWeakPtr(), old_version, new_version));
  }
}

}  // namespace mitto
