#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/indexed_db/engine.h"

namespace mitto {

IndexedDbConnection::IndexedDbConnection(
    IndexedDbDatabase& database,
    std::unique_ptr<IndexedDbDatabaseCallbacks> callbacks,
    uintptr_t client_token)
    : database_(database.AsWeakPtr()),
      callbacks_(std::move(callbacks)),
      client_token_(client_token) {}

IndexedDbConnection::~IndexedDbConnection() {
  // Dropping a connection that still has transactions aborts them, as the
  // browser does when its remote goes.
  if (IsConnected()) {
    AbortTransactionsAndClose("The connection was closed.");
  }
}

void IndexedDbConnection::ReportBadMessage(const std::string& why) {
  if (callbacks_) {
    callbacks_->BadMessage(why);
  }
}

IndexedDbTransaction* IndexedDbConnection::GetTransaction(uint64_t id) const {
  const auto found = transactions_.find(id);
  return found == transactions_.end() ? nullptr : found->second.get();
}

IndexedDbTransaction* IndexedDbConnection::GetTransactionAndVerifyState(
    uint64_t id,
    std::optional<IndexedDbTransactionMode> required_mode,
    IndexedDbException& code,
    std::u16string& message,
    bool& bad) {
  bad = false;
  if (!IsConnected()) {
    code = IndexedDbException::kUnknownError;
    message = u"Not connected.";
    return nullptr;
  }
  IndexedDbTransaction* const transaction = GetTransaction(id);
  if (!transaction) {
    code = IndexedDbException::kUnknownError;
    message = u"Unknown transaction.";
    return nullptr;
  }
  if (required_mode && transaction->mode() != *required_mode) {
    bad = true;
    code = IndexedDbException::kUnknownError;
    message = u"Wrong transaction type.";
    return nullptr;
  }
  if (!transaction->IsAcceptingRequests()) {
    code = IndexedDbException::kIgnorableAbortError;
    message = u"Transaction may have been aborted.";
    return nullptr;
  }
  return transaction;
}

IndexedDbTransaction* IndexedDbConnection::CreateVersionChangeTransaction(
    uint64_t id) {
  CHECK(!GetTransaction(id));
  auto transaction = std::make_unique<IndexedDbTransaction>(
      id, *this, std::set<int64_t>(), IndexedDbTransactionMode::kVersionChange,
      IndexedDbDurability::kStrict);
  IndexedDbTransaction* const pointer = transaction.get();
  transactions_.emplace(id, std::move(transaction));
  return pointer;
}

void IndexedDbConnection::CreateTransaction(uint64_t id,
                                            const std::vector<int64_t>& scope,
                                            IndexedDbTransactionMode mode,
                                            IndexedDbDurability durability) {
  if (!IsConnected()) {
    return;
  }
  if (mode != IndexedDbTransactionMode::kReadOnly &&
      mode != IndexedDbTransactionMode::kReadWrite) {
    ReportBadMessage(
        "a transaction of a mode that is not readonly or "
        "readwrite");
    return;
  }
  if (GetTransaction(id)) {
    ReportBadMessage("a transaction that is already there");
    return;
  }
  // The default bucket's durability is relaxed.
  if (durability == IndexedDbDurability::kDefault) {
    durability = IndexedDbDurability::kRelaxed;
  }
  auto transaction = std::make_unique<IndexedDbTransaction>(
      id, *this, std::set<int64_t>(scope.begin(), scope.end()), mode,
      durability);
  IndexedDbTransaction& added = *transaction;
  transactions_.emplace(id, std::move(transaction));
  database_->RegisterAndScheduleTransaction(added);
}

IndexedDbTransaction* IndexedDbConnection::VerifyTransaction(
    uint64_t id,
    std::optional<IndexedDbTransactionMode> required_mode,
    IndexedDbException* code,
    std::u16string* message) {
  IndexedDbException ignored_code;
  std::u16string ignored_message;
  bool bad = false;
  IndexedDbTransaction* const transaction = GetTransactionAndVerifyState(
      id, required_mode, code ? *code : ignored_code,
      message ? *message : ignored_message, bad);
  if (bad) {
    ReportBadMessage("a call of a transaction of another mode");
  }
  return transaction;
}

void IndexedDbConnection::RenameObjectStore(uint64_t transaction_id,
                                            int64_t object_store_id,
                                            std::u16string new_name) {
  IndexedDbTransaction* const transaction = VerifyTransaction(
      transaction_id, IndexedDbTransactionMode::kVersionChange);
  if (!transaction) {
    return;
  }
  transaction->ScheduleTask(
      IndexedDbTaskType::kPreemptive,
      BindWeakOperation(&IndexedDbDatabase::RenameObjectStoreOperation,
                        database_, object_store_id, std::move(new_name)));
}

void IndexedDbConnection::VersionChangeIgnored() {
  if (!IsConnected()) {
    return;
  }
  database_->VersionChangeIgnored();
}

void IndexedDbConnection::Get(uint64_t transaction_id,
                              int64_t object_store_id,
                              int64_t index_id,
                              IndexedDbKeyRange key_range,
                              bool key_only,
                              IndexedDbReply reply) {
  PageIndexedDbCallback error;
  error.op = IndexedDbCallbackOp::kGetResult;
  IndexedDbTransaction* const transaction = VerifyTransaction(
      transaction_id, std::nullopt, &error.error, &error.message);
  if (!transaction) {
    std::move(reply).Run(std::move(error));
    return;
  }
  transaction->ScheduleTask(BindWeakOperation(
      &IndexedDbDatabase::GetOperation, database_, object_store_id, index_id,
      std::move(key_range), key_only,
      AbortOnDestruct(std::move(reply), IndexedDbCallbackOp::kGetResult,
                      transaction->AsWeakPtr())));
}

void IndexedDbConnection::GetAll(
    uint64_t transaction_id,
    int64_t object_store_id,
    int64_t index_id,
    IndexedDbKeyRange key_range,
    IndexedDbGetAllResultType result_type,
    uint32_t max_count,
    IndexedDbCursorDirection direction,
    base::RepeatingCallback<void(PageIndexedDbCallback)> sink) {
  if (max_count == 0) {
    ReportBadMessage("max_count must be greater than 0.");
    return;
  }
  PageIndexedDbCallback error;
  error.op = IndexedDbCallbackOp::kGetAllResult;
  IndexedDbTransaction* const transaction = VerifyTransaction(
      transaction_id, std::nullopt, &error.error, &error.message);
  if (!transaction) {
    error.done = true;
    sink.Run(std::move(error));
    return;
  }
  transaction->ScheduleTask(BindWeakOperation(
      &IndexedDbDatabase::GetAllOperation, database_, object_store_id, index_id,
      std::move(key_range), result_type, max_count, direction,
      std::make_unique<IndexedDbGetAllSink>(std::move(sink),
                                            transaction->AsWeakPtr())));
}

void IndexedDbConnection::OpenCursor(
    uint64_t transaction_id,
    int64_t object_store_id,
    int64_t index_id,
    IndexedDbKeyRange key_range,
    IndexedDbCursorDirection direction,
    bool key_only,
    IndexedDbTaskType task_type,
    base::OnceCallback<void(std::unique_ptr<IndexedDbCursor>)> adopt_cursor,
    IndexedDbReply reply) {
  PageIndexedDbCallback error;
  error.op = IndexedDbCallbackOp::kOpenCursorResult;
  IndexedDbTransaction* const transaction = VerifyTransaction(
      transaction_id, std::nullopt, &error.error, &error.message);
  if (!transaction) {
    std::move(reply).Run(std::move(error));
    return;
  }
  reply =
      AbortOnDestruct(std::move(reply), IndexedDbCallbackOp::kOpenCursorResult,
                      transaction->AsWeakPtr());
  if (transaction->mode() != IndexedDbTransactionMode::kVersionChange &&
      task_type == IndexedDbTaskType::kPreemptive) {
    ReportBadMessage(
        "OpenCursor with |Preemptive| task type must be called from a version "
        "change transaction.");
    return;
  }
  if (task_type == IndexedDbTaskType::kPreemptive &&
      (index_id != IndexedDbIndexMetadata::kInvalidId || key_only)) {
    ReportBadMessage(
        "OpenCursor with |Preemptive| task type can only be called when "
        "iterating over object store values (to populate an index).");
    return;
  }
  transaction->ScheduleTask(BindWeakOperation(
      &IndexedDbDatabase::OpenCursorOperation, database_, object_store_id,
      index_id, std::move(key_range), direction, key_only, task_type,
      std::move(adopt_cursor), std::move(reply)));
}

void IndexedDbConnection::Count(uint64_t transaction_id,
                                int64_t object_store_id,
                                int64_t index_id,
                                IndexedDbKeyRange key_range,
                                IndexedDbReply reply) {
  PageIndexedDbCallback failed;
  failed.op = IndexedDbCallbackOp::kCountResult;
  reply = InvokeIfNotRun(std::move(reply), std::move(failed));
  IndexedDbTransaction* const transaction =
      VerifyTransaction(transaction_id, std::nullopt);
  if (!transaction) {
    return;
  }
  transaction->ScheduleTask(BindWeakOperation(
      &IndexedDbDatabase::CountOperation, database_, object_store_id, index_id,
      std::move(key_range), std::move(reply)));
}

void IndexedDbConnection::DeleteRange(uint64_t transaction_id,
                                      int64_t object_store_id,
                                      IndexedDbKeyRange key_range,
                                      IndexedDbReply reply) {
  PageIndexedDbCallback failed;
  failed.op = IndexedDbCallbackOp::kSuccessResult;
  reply = InvokeIfNotRun(std::move(reply), std::move(failed));
  IndexedDbTransaction* const transaction =
      VerifyTransaction(transaction_id, std::nullopt);
  if (!transaction) {
    return;
  }
  transaction->ScheduleTask(BindWeakOperation(
      &IndexedDbDatabase::DeleteRangeOperation, database_, object_store_id,
      std::move(key_range), std::move(reply)));
}

void IndexedDbConnection::GetKeyGeneratorCurrentNumber(uint64_t transaction_id,
                                                       int64_t object_store_id,
                                                       IndexedDbReply reply) {
  PageIndexedDbCallback error;
  error.op = IndexedDbCallbackOp::kKeyGeneratorResult;
  error.number = -1;
  IndexedDbTransaction* const transaction = VerifyTransaction(
      transaction_id, std::nullopt, &error.error, &error.message);
  if (!transaction) {
    std::move(reply).Run(std::move(error));
    return;
  }
  PageIndexedDbCallback failed;
  failed.op = IndexedDbCallbackOp::kKeyGeneratorResult;
  failed.number = -1;
  failed.error = IndexedDbException::kIgnorableAbortError;
  failed.message = u"Aborting due to unknown failure.";
  transaction->ScheduleTask(BindWeakOperation(
      &IndexedDbDatabase::GetKeyGeneratorCurrentNumberOperation, database_,
      object_store_id, InvokeIfNotRun(std::move(reply), std::move(failed))));
}

void IndexedDbConnection::Clear(uint64_t transaction_id,
                                int64_t object_store_id,
                                IndexedDbReply reply) {
  PageIndexedDbCallback failed;
  failed.op = IndexedDbCallbackOp::kSuccessResult;
  reply = InvokeIfNotRun(std::move(reply), std::move(failed));
  IndexedDbTransaction* const transaction =
      VerifyTransaction(transaction_id, std::nullopt);
  if (!transaction) {
    return;
  }
  transaction->ScheduleTask(
      BindWeakOperation(&IndexedDbDatabase::ClearOperation, database_,
                        object_store_id, std::move(reply)));
}

void IndexedDbConnection::CreateIndex(uint64_t transaction_id,
                                      int64_t object_store_id,
                                      IndexedDbIndexMetadata index) {
  IndexedDbTransaction* const transaction = VerifyTransaction(
      transaction_id, IndexedDbTransactionMode::kVersionChange);
  if (!transaction) {
    return;
  }
  transaction->ScheduleTask(
      IndexedDbTaskType::kPreemptive,
      BindWeakOperation(&IndexedDbDatabase::CreateIndexOperation, database_,
                        object_store_id, std::move(index)));
}

void IndexedDbConnection::DeleteIndex(uint64_t transaction_id,
                                      int64_t object_store_id,
                                      int64_t index_id) {
  IndexedDbTransaction* const transaction = VerifyTransaction(
      transaction_id, IndexedDbTransactionMode::kVersionChange);
  if (!transaction) {
    return;
  }
  transaction->ScheduleTask(
      BindWeakOperation(&IndexedDbDatabase::DeleteIndexOperation, database_,
                        object_store_id, index_id));
}

void IndexedDbConnection::RenameIndex(uint64_t transaction_id,
                                      int64_t object_store_id,
                                      int64_t index_id,
                                      std::u16string new_name) {
  IndexedDbTransaction* const transaction = VerifyTransaction(
      transaction_id, IndexedDbTransactionMode::kVersionChange);
  if (!transaction) {
    return;
  }
  transaction->ScheduleTask(
      BindWeakOperation(&IndexedDbDatabase::RenameIndexOperation, database_,
                        object_store_id, index_id, std::move(new_name)));
}

void IndexedDbConnection::Abort(uint64_t transaction_id) {
  if (!IsConnected()) {
    return;
  }
  IndexedDbTransaction* const transaction = GetTransaction(transaction_id);
  if (!transaction) {
    return;
  }
  transaction->Abort(IndexedDbException::kAbortError,
                     u"Transaction aborted by user.");
}

void IndexedDbConnection::RemoveTransaction(uint64_t id) {
  transactions_.erase(id);
}

void IndexedDbConnection::AbortAllTransactions(IndexedDbException code,
                                               const std::u16string& message) {
  for (auto& [id, transaction] : transactions_) {
    transaction->Abort(code, message);
  }
}

std::unique_ptr<IndexedDbDatabaseCallbacks>
IndexedDbConnection::AbortTransactionsAndClose(const std::string& message) {
  if (!IsConnected()) {
    return nullptr;
  }
  AbortAllTransactions(
      IndexedDbException::kUnknownError,
      base::UTF8ToUTF16("Connection is closing because of: " + message));
  std::unique_ptr<IndexedDbDatabaseCallbacks> callbacks = std::move(callbacks_);
  database_->ConnectionClosed(*this, std::move(on_close_during_upgrade_));
  return callbacks;
}

void IndexedDbConnection::CloseAndReportForceClose(const std::string& message) {
  if (!IsConnected()) {
    return;
  }
  AbortTransactionsAndClose(message)->ForcedClose();
}

bool IndexedDbConnection::IsHoldingLocks(
    const std::vector<IndexedDbLockId>& lock_ids) const {
  for (const auto& [id, transaction] : transactions_) {
    for (const IndexedDbLockId& held : transaction->lock_ids()) {
      if (std::ranges::find(lock_ids, held) != lock_ids.end()) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace mitto
