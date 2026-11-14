#include "mitto/page_indexed_db.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/notreached.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "mitto/indexed_db/engine.h"
#include "mitto/indexed_db/indexed_db_context.h"
#include "mitto/page_messages.h"

namespace mitto {
namespace {

bool HasCallNumber(IndexedDbCallOp op) {
  switch (op) {
    case IndexedDbCallOp::kGetDatabaseInfo:
    case IndexedDbCallOp::kOpen:
    case IndexedDbCallOp::kDeleteDatabase:
    case IndexedDbCallOp::kGet:
    case IndexedDbCallOp::kGetAll:
    case IndexedDbCallOp::kOpenCursor:
    case IndexedDbCallOp::kCount:
    case IndexedDbCallOp::kDeleteRange:
    case IndexedDbCallOp::kGetKeyGeneratorCurrentNumber:
    case IndexedDbCallOp::kClear:
    case IndexedDbCallOp::kPut:
    case IndexedDbCallOp::kCursorAdvance:
    case IndexedDbCallOp::kCursorContinue:
    case IndexedDbCallOp::kCursorPrefetch:
      return true;
    default:
      return false;
  }
}

}  // namespace

// blink.mojom.IDBFactoryClient of one open or delete.
class PageIndexedDb::FactoryClient final : public IndexedDbFactoryClient {
 public:
  FactoryClient(base::WeakPtr<PageIndexedDb> host,
                uint64_t call,
                uint64_t connection,
                uint64_t transaction)
      : host_(std::move(host)),
        call_(call),
        connection_(connection),
        transaction_(transaction) {}

  ~FactoryClient() override {
    // An open that never gave its connection frees the numbers.
    if (host_ && connection_ != 0 && !gave_connection_) {
      host_->connections_.erase(connection_);
      host_->transactions_.erase(transaction_);
    }
  }

  bool IsConnected() const override { return !!host_; }

  void Error(IndexedDbException code, std::u16string message) override {
    PageIndexedDbCallback callback;
    callback.op = IndexedDbCallbackOp::kError;
    callback.error = code;
    callback.message = std::move(message);
    Send(std::move(callback));
  }

  void Blocked(int64_t old_version) override {
    PageIndexedDbCallback callback;
    callback.op = IndexedDbCallbackOp::kBlocked;
    callback.old_version = old_version;
    Send(std::move(callback));
  }

  void UpgradeNeeded(std::unique_ptr<IndexedDbConnection> connection,
                     int64_t old_version,
                     const IndexedDbDatabaseMetadata& metadata) override {
    if (!host_) {
      return;
    }
    gave_connection_ = true;
    host_->AdoptConnection(connection_, std::move(connection));
    PageIndexedDbCallback callback;
    callback.op = IndexedDbCallbackOp::kUpgradeNeeded;
    callback.old_version = old_version;
    callback.metadata = metadata;
    Send(std::move(callback));
  }

  void OpenSuccess(std::unique_ptr<IndexedDbConnection> connection,
                   const IndexedDbDatabaseMetadata& metadata) override {
    if (!host_) {
      return;
    }
    if (connection) {
      // Opened without an upgrade: its transaction's number was never used.
      gave_connection_ = true;
      host_->transactions_.erase(transaction_);
      host_->AdoptConnection(connection_, std::move(connection));
    }
    PageIndexedDbCallback callback;
    callback.op = IndexedDbCallbackOp::kOpenSuccess;
    callback.metadata = metadata;
    Send(std::move(callback));
  }

  void DeleteSuccess(int64_t old_version) override {
    PageIndexedDbCallback callback;
    callback.op = IndexedDbCallbackOp::kDeleteSuccess;
    callback.old_version = old_version;
    Send(std::move(callback));
  }

 private:
  void Send(PageIndexedDbCallback callback) {
    if (host_) {
      host_->SendReply(call_, std::move(callback));
    }
  }

  const base::WeakPtr<PageIndexedDb> host_;
  const uint64_t call_;
  // An open's: the numbers the Page gave its connection and its upgrade's
  // transaction. 0 for a delete.
  const uint64_t connection_;
  const uint64_t transaction_;
  bool gave_connection_ = false;
};

// blink.mojom.IDBDatabaseCallbacks of one connection.
class PageIndexedDb::DatabaseCallbacks final
    : public IndexedDbDatabaseCallbacks {
 public:
  DatabaseCallbacks(base::WeakPtr<PageIndexedDb> host, uint64_t connection)
      : host_(std::move(host)), connection_(connection) {}

  void ForcedClose() override {
    PageIndexedDbCallback callback;
    callback.op = IndexedDbCallbackOp::kForcedClose;
    Send(std::move(callback));
  }

  void VersionChange(int64_t old_version, int64_t new_version) override {
    PageIndexedDbCallback callback;
    callback.op = IndexedDbCallbackOp::kVersionChange;
    callback.old_version = old_version;
    callback.new_version = new_version;
    Send(std::move(callback));
  }

  void Abort(uint64_t transaction_id,
             IndexedDbException code,
             const std::u16string& message) override {
    if (host_) {
      host_->TransactionFinished(transaction_id);
    }
    PageIndexedDbCallback callback;
    callback.op = IndexedDbCallbackOp::kAbort;
    callback.transaction = transaction_id;
    callback.error = code;
    callback.message = message;
    Send(std::move(callback));
  }

  void Complete(uint64_t transaction_id) override {
    if (host_) {
      host_->TransactionFinished(transaction_id);
    }
    PageIndexedDbCallback callback;
    callback.op = IndexedDbCallbackOp::kComplete;
    callback.transaction = transaction_id;
    Send(std::move(callback));
  }

  void BadMessage(const std::string& why) override {
    if (host_) {
      host_->RefuseCall(why);
    }
  }

 private:
  void Send(PageIndexedDbCallback callback) {
    if (host_) {
      callback.connection = connection_;
      host_->SendCallback(std::move(callback));
    }
  }

  const base::WeakPtr<PageIndexedDb> host_;
  const uint64_t connection_;
};

PageIndexedDb::PageIndexedDb(IndexedDbContext& context,
                             Send send,
                             Refuse refuse)
    : context_(context), send_(std::move(send)), refuse_(std::move(refuse)) {}

PageIndexedDb::~PageIndexedDb() {
  // Nothing more reaches the Page, nor this.
  weak_factory_.InvalidateWeakPtrs();
  cursors_.clear();
  std::map<uint64_t, std::unique_ptr<IndexedDbConnection>> connections =
      std::move(connections_);
  connections_.clear();
  connections.clear();
}

void PageIndexedDb::OnCall(PageIndexedDbCall call,
                           const std::optional<std::string>& storage_key) {
  if (HasCallNumber(call.op)) {
    if (call.call <= last_call_) {
      RefuseCall(
          base::StrCat({"IndexedDB call ", base::NumberToString(call.call),
                        " after call ", base::NumberToString(last_call_)}));
      return;
    }
    last_call_ = call.call;
  }
  switch (call.op) {
    case IndexedDbCallOp::kGetDatabaseInfo:
    case IndexedDbCallOp::kOpen:
    case IndexedDbCallOp::kDeleteDatabase:
      OnFactoryCall(std::move(call), storage_key);
      return;
    case IndexedDbCallOp::kCreateObjectStore:
    case IndexedDbCallOp::kDeleteObjectStore:
    case IndexedDbCallOp::kPut:
    case IndexedDbCallOp::kSetIndexKeys:
    case IndexedDbCallOp::kSetIndexKeysDone:
    case IndexedDbCallOp::kCommit:
      OnTransactionCall(std::move(call));
      return;
    case IndexedDbCallOp::kCursorAdvance:
    case IndexedDbCallOp::kCursorContinue:
    case IndexedDbCallOp::kCursorPrefetch:
    case IndexedDbCallOp::kCursorPrefetchReset:
    case IndexedDbCallOp::kCloseCursor:
      OnCursorCall(std::move(call));
      return;
    default:
      break;
  }
  const auto found = connections_.find(call.connection);
  if (found == connections_.end() || !found->second) {
    RefuseCall(base::StrCat({"an IndexedDB call of connection ",
                             base::NumberToString(call.connection),
                             ", which the Page does not have"}));
    return;
  }
  OnDatabaseCall(std::move(call), *found->second);
}

void PageIndexedDb::OnFactoryCall(
    PageIndexedDbCall call,
    const std::optional<std::string>& storage_key) {
  if (!storage_key) {
    RefuseCall("IndexedDB of a document whose origin may not reach it");
    return;
  }
  IndexedDbBucket& bucket = context_->ForKey(*storage_key);
  switch (call.op) {
    case IndexedDbCallOp::kGetDatabaseInfo:
      bucket.GetDatabaseInfo(ReplyTo(call.call));
      return;
    case IndexedDbCallOp::kOpen: {
      if (call.version < 1 &&
          call.version != IndexedDbDatabaseMetadata::kNoVersion) {
        RefuseCall("an IndexedDB open of a version that is not valid");
        return;
      }
      if (ConnectionInUse(call.connection) ||
          transactions_.contains(call.transaction)) {
        RefuseCall(base::StrCat({"an IndexedDB open with connection ",
                                 base::NumberToString(call.connection),
                                 " or transaction ",
                                 base::NumberToString(call.transaction),
                                 ", which the Page uses already"}));
        return;
      }
      connections_.emplace(call.connection, nullptr);
      transactions_.emplace(call.transaction, call.connection);
      const base::WeakPtr<PageIndexedDb> self = weak_factory_.GetWeakPtr();
      bucket.Open(std::move(call.name), call.version, call.transaction,
                  reinterpret_cast<uintptr_t>(this),
                  std::make_unique<FactoryClient>(
                      self, call.call, call.connection, call.transaction),
                  std::make_unique<DatabaseCallbacks>(self, call.connection));
      return;
    }
    case IndexedDbCallOp::kDeleteDatabase:
      bucket.DeleteDatabase(
          std::move(call.name),
          std::make_unique<FactoryClient>(weak_factory_.GetWeakPtr(), call.call,
                                          /*connection=*/0,
                                          /*transaction=*/0));
      return;
    default:
      NOTREACHED();
  }
}

void PageIndexedDb::OnDatabaseCall(PageIndexedDbCall call,
                                   IndexedDbConnection& connection) {
  switch (call.op) {
    case IndexedDbCallOp::kCreateTransaction:
      if (transactions_.contains(call.transaction)) {
        RefuseCall(base::StrCat({"IndexedDB transaction ",
                                 base::NumberToString(call.transaction),
                                 ", which the Page uses already"}));
        return;
      }
      if (connection.IsConnected()) {
        transactions_.emplace(call.transaction, call.connection);
      }
      connection.CreateTransaction(call.transaction, call.scope, call.mode,
                                   call.durability);
      return;
    case IndexedDbCallOp::kVersionChangeIgnored:
      connection.VersionChangeIgnored();
      return;
    case IndexedDbCallOp::kGet:
      connection.Get(call.transaction, call.object_store_id, call.index_id,
                     std::move(call.key_range), call.key_only,
                     ReplyTo(call.call));
      return;
    case IndexedDbCallOp::kGetAll:
      connection.GetAll(
          call.transaction, call.object_store_id, call.index_id,
          std::move(call.key_range), call.result_type, call.count,
          call.direction,
          base::BindRepeating(&PageIndexedDb::SendReply,
                              weak_factory_.GetWeakPtr(), call.call));
      return;
    case IndexedDbCallOp::kOpenCursor:
      if (cursors_.contains(call.cursor)) {
        RefuseCall(base::StrCat({"IndexedDB cursor ",
                                 base::NumberToString(call.cursor),
                                 ", which the Page uses already"}));
        return;
      }
      connection.OpenCursor(
          call.transaction, call.object_store_id, call.index_id,
          std::move(call.key_range), call.direction, call.key_only,
          call.task_type,
          base::BindOnce(&PageIndexedDb::AdoptCursor,
                         weak_factory_.GetWeakPtr(), call.cursor),
          ReplyTo(call.call));
      return;
    case IndexedDbCallOp::kCount:
      connection.Count(call.transaction, call.object_store_id, call.index_id,
                       std::move(call.key_range), ReplyTo(call.call));
      return;
    case IndexedDbCallOp::kDeleteRange:
      connection.DeleteRange(call.transaction, call.object_store_id,
                             std::move(call.key_range), ReplyTo(call.call));
      return;
    case IndexedDbCallOp::kGetKeyGeneratorCurrentNumber:
      connection.GetKeyGeneratorCurrentNumber(
          call.transaction, call.object_store_id, ReplyTo(call.call));
      return;
    case IndexedDbCallOp::kClear:
      connection.Clear(call.transaction, call.object_store_id,
                       ReplyTo(call.call));
      return;
    case IndexedDbCallOp::kCreateIndex:
      connection.CreateIndex(call.transaction, call.object_store_id,
                             std::move(call.index));
      return;
    case IndexedDbCallOp::kDeleteIndex:
      connection.DeleteIndex(call.transaction, call.object_store_id,
                             call.index_id);
      return;
    case IndexedDbCallOp::kRenameIndex:
      connection.RenameIndex(call.transaction, call.object_store_id,
                             call.index_id, std::move(call.name));
      return;
    case IndexedDbCallOp::kRenameObjectStore:
      connection.RenameObjectStore(call.transaction, call.object_store_id,
                                   std::move(call.name));
      return;
    case IndexedDbCallOp::kAbort:
      connection.Abort(call.transaction);
      return;
    case IndexedDbCallOp::kDidBecomeInactive:
      // A Page is never kept inactive, as a frame in the back/forward cache
      // is.
      return;
    case IndexedDbCallOp::kCloseConnection: {
      std::unique_ptr<IndexedDbConnection> closed =
          std::move(connections_.at(call.connection));
      connections_.erase(call.connection);
      std::erase_if(transactions_, [&call](const auto& entry) {
        return entry.second == call.connection;
      });
      // Closing aborts what the connection still runs, as a dropped remote
      // does.
      closed.reset();
      return;
    }
    default:
      NOTREACHED();
  }
}

void PageIndexedDb::OnTransactionCall(PageIndexedDbCall call) {
  IndexedDbTransaction* const transaction = FindTransaction(call.transaction);
  // A transaction that finished has gone with its interface, which drops
  // what comes after.
  if (!transaction) {
    return;
  }
  switch (call.op) {
    case IndexedDbCallOp::kCreateObjectStore:
      transaction->CreateObjectStore(call.object_store_id, std::move(call.name),
                                     std::move(call.key_path),
                                     call.auto_increment);
      return;
    case IndexedDbCallOp::kDeleteObjectStore:
      transaction->DeleteObjectStore(call.object_store_id);
      return;
    case IndexedDbCallOp::kPut:
      transaction->Put(call.object_store_id, std::move(call.value),
                       std::move(call.key), call.put_mode,
                       std::move(call.index_keys), ReplyTo(call.call));
      return;
    case IndexedDbCallOp::kSetIndexKeys:
      transaction->SetIndexKeys(call.object_store_id,
                                std::move(call.primary_key),
                                std::move(call.index_keys));
      return;
    case IndexedDbCallOp::kSetIndexKeysDone:
      transaction->SetIndexKeysDone();
      return;
    case IndexedDbCallOp::kCommit:
      transaction->Commit(call.num_errors_handled);
      return;
    default:
      NOTREACHED();
  }
}

void PageIndexedDb::OnCursorCall(PageIndexedDbCall call) {
  const auto found = cursors_.find(call.cursor);
  if (found == cursors_.end()) {
    if (call.op != IndexedDbCallOp::kCloseCursor) {
      RefuseCall(base::StrCat({"a call of IndexedDB cursor ",
                               base::NumberToString(call.cursor),
                               ", which the Page does not have"}));
    }
    return;
  }
  IndexedDbCursor& cursor = *found->second;
  bool valid = true;
  switch (call.op) {
    case IndexedDbCallOp::kCursorAdvance:
      valid = cursor.Advance(call.count, ReplyTo(call.call));
      break;
    case IndexedDbCallOp::kCursorContinue:
      valid = cursor.Continue(std::move(call.key), std::move(call.primary_key),
                              ReplyTo(call.call));
      break;
    case IndexedDbCallOp::kCursorPrefetch:
      cursor.Prefetch(call.count, ReplyTo(call.call));
      break;
    case IndexedDbCallOp::kCursorPrefetchReset:
      valid = cursor.PrefetchReset(call.count);
      break;
    case IndexedDbCallOp::kCloseCursor:
      cursors_.erase(found);
      return;
    default:
      NOTREACHED();
  }
  if (!valid) {
    RefuseCall(base::StrCat({"a call of IndexedDB cursor ",
                             base::NumberToString(call.cursor),
                             " that no renderer makes"}));
  }
}

bool PageIndexedDb::ConnectionInUse(uint64_t connection) const {
  return connections_.contains(connection);
}

IndexedDbTransaction* PageIndexedDb::FindTransaction(uint64_t transaction) {
  const auto found = transactions_.find(transaction);
  if (found == transactions_.end()) {
    return nullptr;
  }
  const auto connection = connections_.find(found->second);
  if (connection == connections_.end() || !connection->second) {
    return nullptr;
  }
  return connection->second->GetTransaction(transaction);
}

void PageIndexedDb::AdoptConnection(
    uint64_t id,
    std::unique_ptr<IndexedDbConnection> connection) {
  connections_[id] = std::move(connection);
}

void PageIndexedDb::AdoptCursor(uint64_t id,
                                std::unique_ptr<IndexedDbCursor> cursor) {
  if (cursors_.contains(id)) {
    RefuseCall(base::StrCat({"IndexedDB cursor ", base::NumberToString(id),
                             ", which the Page uses already"}));
    return;
  }
  cursors_.emplace(id, std::move(cursor));
}

void PageIndexedDb::TransactionFinished(uint64_t transaction) {
  transactions_.erase(transaction);
}

base::OnceCallback<void(PageIndexedDbCallback)> PageIndexedDb::ReplyTo(
    uint64_t call) {
  return base::BindOnce(&PageIndexedDb::SendReply, weak_factory_.GetWeakPtr(),
                        call);
}

void PageIndexedDb::SendReply(uint64_t call, PageIndexedDbCallback answer) {
  answer.call = call;
  SendCallback(std::move(answer));
}

void PageIndexedDb::SendCallback(PageIndexedDbCallback callback) {
  if (!IndexedDbCallbackFits(callback)) {
    // Only an entry far past what Chrome sends at once gets here: what the
    // Page hears instead is an error of the call.
    PageIndexedDbCallback error;
    error.op = callback.op;
    error.call = callback.call;
    error.connection = callback.connection;
    error.transaction = callback.transaction;
    error.error = IndexedDbException::kUnknownError;
    error.message = u"The result is too large.";
    error.done = true;
    callback = std::move(error);
  }
  send_.Run(EncodeFrame(PageMessage::kIndexedDbCallback,
                        EncodeIndexedDbCallback(callback)));
}

void PageIndexedDb::RefuseCall(std::string why) {
  refuse_.Run(std::move(why));
}

}  // namespace mitto
