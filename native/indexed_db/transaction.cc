#include <algorithm>
#include <cmath>
#include <utility>

#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/strcat.h"
#include "base/time/time.h"
#include "mitto/indexed_db/engine.h"

namespace mitto {
namespace {

// How long a transaction that blocks another client may sit idle before it
// aborts.
constexpr base::TimeDelta kInactivityTimeout = base::Seconds(60);

// The largest integer that an ECMAScript number holds exactly, past which a
// key generator stops.
constexpr int64_t kMaxGeneratorValue = 9007199254740992LL;

// A reply that answers with what `make_answer` makes if it goes unrun.
class ReplyOnDestruct {
 public:
  ReplyOnDestruct(IndexedDbReply reply,
                  base::OnceCallback<PageIndexedDbCallback()> make_answer)
      : reply_(std::move(reply)), make_answer_(std::move(make_answer)) {}
  ReplyOnDestruct(const ReplyOnDestruct&) = delete;
  ReplyOnDestruct& operator=(const ReplyOnDestruct&) = delete;
  ~ReplyOnDestruct() {
    if (reply_) {
      std::move(reply_).Run(std::move(make_answer_).Run());
    }
  }

  void Run(PageIndexedDbCallback answer) {
    std::move(reply_).Run(std::move(answer));
  }

 private:
  IndexedDbReply reply_;
  base::OnceCallback<PageIndexedDbCallback()> make_answer_;
};

IndexedDbReply WrapReply(
    IndexedDbReply reply,
    base::OnceCallback<PageIndexedDbCallback()> make_answer) {
  return base::BindOnce(&ReplyOnDestruct::Run,
                        std::make_unique<ReplyOnDestruct>(
                            std::move(reply), std::move(make_answer)));
}

}  // namespace

IndexedDbReply AbortOnDestruct(
    IndexedDbReply reply,
    IndexedDbCallbackOp op,
    base::WeakPtr<IndexedDbTransaction> transaction) {
  return WrapReply(std::move(reply),
                   base::BindOnce(
                       [](IndexedDbCallbackOp op,
                          base::WeakPtr<IndexedDbTransaction> transaction) {
                         if (transaction) {
                           transaction->IncrementNumErrorsSent();
                         }
                         PageIndexedDbCallback answer;
                         answer.op = op;
                         answer.error =
                             IndexedDbException::kIgnorableAbortError;
                         answer.message = u"Backend aborted error";
                         return answer;
                       },
                       op, std::move(transaction)));
}

IndexedDbReply InvokeIfNotRun(IndexedDbReply reply,
                              PageIndexedDbCallback answer) {
  return WrapReply(
      std::move(reply),
      base::BindOnce([](PageIndexedDbCallback answer) { return answer; },
                     std::move(answer)));
}

IndexedDbTransaction::IndexedDbTransaction(uint64_t id,
                                           IndexedDbConnection& connection,
                                           std::set<int64_t> scope,
                                           IndexedDbTransactionMode mode,
                                           IndexedDbDurability durability)
    : id_(id),
      connection_(connection),
      scope_(std::move(scope)),
      mode_(mode),
      durability_(durability),
      backing_store_(std::make_unique<IndexedDbBackingStore::Transaction>(
          connection.database().bucket().backing_store(),
          connection.database().id())) {
  if (mode_ == IndexedDbTransactionMode::kVersionChange) {
    metadata_before_ = connection.database().metadata();
  }
}

IndexedDbTransaction::~IndexedDbTransaction() {
  CloseOpenCursors();
}

IndexedDbDatabase& IndexedDbTransaction::database() {
  return connection_->database();
}

void IndexedDbTransaction::ScheduleTask(IndexedDbTaskType type,
                                        Operation operation) {
  if (state_ == kFinished) {
    return;
  }
  timeout_timer_.Stop();
  used_ = true;
  if (type == IndexedDbTaskType::kNormal) {
    task_queue_.push(std::move(operation));
  } else {
    preemptive_task_queue_.push(std::move(operation));
  }
  if (state_ == kStarted) {
    database().bucket().QueueRunTasks();
  }
}

void IndexedDbTransaction::Start() {
  // It may have aborted after the start was posted.
  if (state_ == kFinished) {
    return;
  }
  CHECK_EQ(state_, kCreated);
  state_ = kStarted;
  database().bucket().QueueRunTasks();
}

void IndexedDbTransaction::Abort(IndexedDbException code,
                                 std::u16string message) {
  if (state_ == kFinished) {
    return;
  }
  aborted_ = true;
  timeout_timer_.Stop();
  state_ = kFinished;
  CloseOpenCursors();
  backing_store_->Rollback();
  // Dropping the operations answers each that was waiting.
  preemptive_task_queue_ = {};
  pending_preemptive_events_ = 0;
  task_queue_ = {};
  locks_holder_.locks.clear();
  locks_holder_.CancelLockRequest();
  if (metadata_before_) {
    database().metadata() = std::move(*metadata_before_);
    metadata_before_.reset();
  }
  if (IndexedDbDatabaseCallbacks* const callbacks = connection_->callbacks()) {
    callbacks->Abort(id_, code, message);
  }
  database().bucket().QueueRunTasks();
}

bool IndexedDbTransaction::HasPendingTasks() const {
  return pending_preemptive_events_ != 0 || !task_queue_.empty() ||
         !preemptive_task_queue_.empty();
}

void IndexedDbTransaction::SetCommitFlag() {
  // An abort may have come first.
  if (!IsAcceptingRequests()) {
    return;
  }
  is_commit_pending_ = true;
  database().bucket().QueueRunTasks();
}

void IndexedDbTransaction::Commit(int64_t num_errors_handled) {
  if (!IsAcceptingRequests() || !connection_->IsConnected()) {
    return;
  }
  num_errors_handled_ = num_errors_handled;
  // An empty transaction, or one that only removes, always may.
  if (preliminary_size_estimate_ <= 0) {
    SetCommitFlag();
    return;
  }
  database().bucket().quota().CheckCanUseDiskSpace(
      preliminary_size_estimate_,
      base::BindOnce(&IndexedDbTransaction::OnQuotaCheckDone,
                     weak_factory_.GetWeakPtr()));
}

void IndexedDbTransaction::OnQuotaCheckDone(bool allowed) {
  if (!connection_->IsConnected()) {
    return;
  }
  if (allowed) {
    SetCommitFlag();
  } else {
    Abort(IndexedDbException::kQuotaError, std::u16string());
  }
}

bool IndexedDbTransaction::DoPendingCommit() {
  CHECK(is_commit_pending_);
  timeout_timer_.Stop();
  if (state_ != kStarted || HasPendingTasks()) {
    return true;
  }
  // More errors sent than the page handled: it is not known whether it
  // would have handled the rest.
  if (num_errors_sent_ != num_errors_handled_) {
    is_commit_pending_ = false;
    Abort(IndexedDbException::kUnknownError, std::u16string());
    return true;
  }
  state_ = kCommitting;
  return CommitPhaseTwo();
}

bool IndexedDbTransaction::CommitPhaseTwo() {
  if (state_ == kFinished) {
    return true;
  }
  CHECK_EQ(state_, kCommitting);
  state_ = kFinished;
  const bool committed = !used_ || backing_store_->Commit();
  CloseOpenCursors();
  locks_holder_.locks.clear();
  metadata_before_.reset();
  if (committed) {
    if (used_) {
      database().bucket().UpdateUsage();
    }
    if (IndexedDbDatabaseCallbacks* const callbacks =
            connection_->callbacks()) {
      callbacks->Complete(id_);
    }
    return true;
  }
  if (IndexedDbDatabaseCallbacks* const callbacks = connection_->callbacks()) {
    callbacks->Abort(id_, IndexedDbException::kUnknownError,
                     u"Internal error committing transaction.");
  }
  return true;
}

bool IndexedDbTransaction::RunTasks() {
  CHECK(!processing_);
  CHECK_NE(state_, kFinished);
  if (task_queue_.empty() && preemptive_task_queue_.empty() &&
      !is_commit_pending_) {
    return true;
  }
  const base::WeakPtr<IndexedDbTransaction> self = weak_factory_.GetWeakPtr();
  processing_ = true;
  base::ScopedClosureRunner reset(base::BindOnce(
      [](base::WeakPtr<IndexedDbTransaction> transaction) {
        if (transaction) {
          transaction->processing_ = false;
        }
      },
      self));
  const auto queue = [this]() {
    return !preemptive_task_queue_.empty() || pending_preemptive_events_ != 0
               ? &preemptive_task_queue_
               : &task_queue_;
  };
  for (base::queue<Operation>* tasks = queue();
       !tasks->empty() && state_ != kFinished; tasks = queue()) {
    Operation operation = std::move(tasks->front());
    tasks->pop();
    if (!std::move(operation).Run(*this)) {
      if (self) {
        Abort(IndexedDbException::kUnknownError,
              u"Internal error running a transaction's task.");
      }
      return false;
    }
    CHECK(self);
  }
  if (!HasPendingTasks() && state_ == kStarted) {
    if (is_commit_pending_) {
      return DoPendingCommit();
    }
    timeout_timer_.Start(
        FROM_HERE, kInactivityTimeout,
        base::BindOnce(&IndexedDbTransaction::OnInactivityTimeout,
                       weak_factory_.GetWeakPtr()));
  }
  return true;
}

void IndexedDbTransaction::OnInactivityTimeout() {
  // Only one that holds what another client waits for times out.
  if (state_ != kStarted || database().OnlyHasOneClient() ||
      !database().lock_manager().IsBlockingAnyRequest(lock_ids())) {
    return;
  }
  Abort(IndexedDbException::kTimeoutError,
        u"Transaction timed out due to inactivity.");
}

void IndexedDbTransaction::RegisterOpenCursor(IndexedDbCursor* cursor) {
  open_cursors_.insert(cursor);
}

void IndexedDbTransaction::UnregisterOpenCursor(IndexedDbCursor* cursor) {
  open_cursors_.erase(cursor);
}

void IndexedDbTransaction::CloseOpenCursors() {
  std::set<raw_ptr<IndexedDbCursor>> cursors = std::move(open_cursors_);
  open_cursors_.clear();
  for (IndexedDbCursor* cursor : cursors) {
    cursor->Close();
  }
}

std::vector<IndexedDbLockId> IndexedDbTransaction::lock_ids() const {
  std::vector<IndexedDbLockId> ids;
  for (const IndexedDbLock& lock : locks_holder_.locks) {
    ids.push_back(lock.id());
  }
  return ids;
}

void IndexedDbTransaction::CreateObjectStore(int64_t object_store_id,
                                             std::u16string name,
                                             IDBKeyPath key_path,
                                             bool auto_increment) {
  if (!connection_->VerifyTransaction(
          id_, IndexedDbTransactionMode::kVersionChange)) {
    return;
  }
  ScheduleTask(
      IndexedDbTaskType::kPreemptive,
      BindWeakOperation(&IndexedDbDatabase::CreateObjectStoreOperation,
                        database().AsWeakPtr(), object_store_id,
                        std::move(name), std::move(key_path), auto_increment));
}

void IndexedDbTransaction::DeleteObjectStore(int64_t object_store_id) {
  if (!connection_->VerifyTransaction(
          id_, IndexedDbTransactionMode::kVersionChange)) {
    return;
  }
  ScheduleTask(BindWeakOperation(&IndexedDbDatabase::DeleteObjectStoreOperation,
                                 database().AsWeakPtr(), object_store_id));
}

void IndexedDbTransaction::Put(int64_t object_store_id,
                               std::string value,
                               IDBKey key,
                               IndexedDbPutMode mode,
                               std::vector<IndexedDbIndexKeys> index_keys,
                               IndexedDbReply reply) {
  if (mode_ == IndexedDbTransactionMode::kReadOnly) {
    connection_->ReportBadMessage("Attempted to Put on readonly txn.");
    return;
  }
  if (!IsAcceptingRequests() || !connection_->IsConnected()) {
    PageIndexedDbCallback answer;
    answer.op = IndexedDbCallbackOp::kPutResult;
    answer.error = IndexedDbException::kUnknownError;
    answer.message = u"Not connected.";
    std::move(reply).Run(std::move(answer));
    return;
  }
  // The transaction's size grows by what the put writes.
  preliminary_size_estimate_ +=
      static_cast<int64_t>(value.size() + key.SizeEstimate());
  ScheduleTask(BindWeakOperation(
      &IndexedDbTransaction::DoPut, AsWeakPtr(), object_store_id,
      std::move(value), std::move(key), mode, std::move(index_keys),
      AbortOnDestruct(std::move(reply), IndexedDbCallbackOp::kPutResult,
                      AsWeakPtr())));
}

bool IndexedDbTransaction::DoPut(int64_t object_store_id,
                                 std::string value,
                                 IDBKey key,
                                 IndexedDbPutMode mode,
                                 std::vector<IndexedDbIndexKeys> index_keys,
                                 IndexedDbReply reply,
                                 IndexedDbTransaction& transaction) {
  const auto error = [this, &reply](IndexedDbException code,
                                    std::u16string message) {
    IncrementNumErrorsSent();
    PageIndexedDbCallback answer;
    answer.op = IndexedDbCallbackOp::kPutResult;
    answer.error = code;
    answer.message = std::move(message);
    std::move(reply).Run(std::move(answer));
    return true;
  };
  const IndexedDbObjectStoreMetadata* const object_store =
      database().GetObjectStoreMetadata(object_store_id);
  if (!object_store) {
    connection_->ReportBadMessage(
        "a put into an object store that is not there");
    return false;
  }
  for (const IndexedDbIndexKeys& keys : index_keys) {
    if (!object_store->indexes.contains(keys.index_id)) {
      connection_->ReportBadMessage(
          "a put with keys of an index that is not there");
      return false;
    }
  }
  bool key_was_generated = false;
  if (mode != IndexedDbPutMode::kCursorUpdate && object_store->auto_increment &&
      !key.IsValid()) {
    const int64_t current =
        backing_store_->GetKeyGeneratorCurrentNumber(object_store_id);
    if (current < 0 || current > kMaxGeneratorValue) {
      return error(IndexedDbException::kConstraintError,
                   u"Maximum key generator value reached.");
    }
    key = IDBKey::Number(static_cast<double>(current));
    key_was_generated = true;
  }
  if (!key.IsValid()) {
    connection_->ReportBadMessage("a put of a key that is not valid");
    return false;
  }
  if (mode == IndexedDbPutMode::kAddOnly &&
      backing_store_->KeyExistsInObjectStore(object_store_id, key)) {
    return error(IndexedDbException::kConstraintError,
                 u"Key already exists in the object store.");
  }
  // MakeIndexWriters(): the primary key that the generator gave goes into the
  // index keys that the renderer could not know it for.
  for (IndexedDbIndexKeys& keys : index_keys) {
    const IndexedDbIndexMetadata& index =
        object_store->indexes.at(keys.index_id);
    if (key_was_generated && !object_store->key_path.IsNull()) {
      if (index.key_path == object_store->key_path) {
        keys.keys.push_back(key);
      } else if (index.key_path.type == IDBKeyPathType::kArray) {
        for (IDBKey& index_key : keys.keys) {
          if (index_key.HasHoles()) {
            index_key = index_key.FillHoles(key);
          }
        }
      }
    }
    for (const IDBKey& index_key : keys.keys) {
      if (!index_key.IsValid()) {
        connection_->ReportBadMessage(
            "a put of an index key that is not valid");
        return false;
      }
      if (index.unique && backing_store_->IndexKeyExistsWithOtherPrimaryKey(
                              object_store_id, index.id, index_key, key)) {
        return error(IndexedDbException::kConstraintError,
                     base::StrCat({u"Unable to add key to index '", index.name,
                                   u"': at least one key does not satisfy the "
                                   u"uniqueness requirements."}));
      }
    }
  }
  backing_store_->PutRecord(object_store_id, key, value);
  for (const IndexedDbIndexKeys& keys : index_keys) {
    backing_store_->PutIndexEntries(object_store_id, keys.index_id, key,
                                    keys.keys);
  }
  if (object_store->auto_increment && mode != IndexedDbPutMode::kCursorUpdate &&
      key.type() == IDBKeyType::kNumber) {
    const int64_t new_max =
        1 + base::saturated_cast<int64_t>(std::floor(std::min(
                key.number(), static_cast<double>(kMaxGeneratorValue))));
    backing_store_->MaybeUpdateKeyGeneratorCurrentNumber(
        object_store_id, new_max, key_was_generated);
  }
  PageIndexedDbCallback answer;
  answer.op = IndexedDbCallbackOp::kPutResult;
  answer.key = std::move(key);
  std::move(reply).Run(std::move(answer));
  return true;
}

void IndexedDbTransaction::SetIndexKeys(
    int64_t object_store_id,
    IDBKey primary_key,
    std::vector<IndexedDbIndexKeys> index_keys) {
  if (!IsAcceptingRequests() || !connection_->IsConnected()) {
    return;
  }
  if (mode_ != IndexedDbTransactionMode::kVersionChange) {
    connection_->ReportBadMessage(
        "SetIndexKeys must be called from a version change transaction.");
    return;
  }
  if (!primary_key.IsValid()) {
    connection_->ReportBadMessage("SetIndexKeys used with invalid key.");
    return;
  }
  ScheduleTask(
      IndexedDbTaskType::kPreemptive,
      base::BindOnce(
          [](int64_t object_store_id, IDBKey primary_key,
             std::vector<IndexedDbIndexKeys> index_keys,
             IndexedDbTransaction& transaction) {
            const IndexedDbObjectStoreMetadata* const object_store =
                transaction.database().GetObjectStoreMetadata(object_store_id);
            if (!object_store) {
              transaction.connection().ReportBadMessage(
                  "index keys of an object store that is not there");
              return false;
            }
            if (!transaction.backing_store().KeyExistsInObjectStore(
                    object_store_id, primary_key)) {
              transaction.Abort(
                  IndexedDbException::kUnknownError,
                  u"Internal error setting index keys for object store.");
              return true;
            }
            for (const IndexedDbIndexKeys& keys : index_keys) {
              const auto index = object_store->indexes.find(keys.index_id);
              if (index == object_store->indexes.end()) {
                transaction.connection().ReportBadMessage(
                    "index keys of an index that is not there");
                return false;
              }
              for (const IDBKey& key : keys.keys) {
                if (!key.IsValid()) {
                  transaction.connection().ReportBadMessage(
                      "an index key that is not valid");
                  return false;
                }
                if (index->second.unique &&
                    transaction.backing_store()
                        .IndexKeyExistsWithOtherPrimaryKey(
                            object_store_id, keys.index_id, key, primary_key)) {
                  transaction.Abort(
                      IndexedDbException::kConstraintError,
                      base::StrCat({u"Unable to add key to index '",
                                    index->second.name,
                                    u"': at least one key does not satisfy "
                                    u"the uniqueness requirements."}));
                  return true;
                }
              }
            }
            for (const IndexedDbIndexKeys& keys : index_keys) {
              transaction.backing_store().PutIndexEntries(
                  object_store_id, keys.index_id, primary_key, keys.keys);
            }
            return true;
          },
          object_store_id, std::move(primary_key), std::move(index_keys)));
}

void IndexedDbTransaction::SetIndexKeysDone() {
  if (!IsAcceptingRequests() || !connection_->IsConnected()) {
    return;
  }
  ScheduleTask(IndexedDbTaskType::kPreemptive,
               base::BindOnce([](IndexedDbTransaction& transaction) {
                 if (transaction.pending_preemptive_events_ == 0) {
                   transaction.connection().ReportBadMessage(
                       "SetIndexKeysDone called without beginning indexing");
                   return false;
                 }
                 --transaction.pending_preemptive_events_;
                 return true;
               }));
}

}  // namespace mitto
