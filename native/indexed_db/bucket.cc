#include <utility>

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "mitto/indexed_db/engine.h"

namespace mitto {

IndexedDbBucket::IndexedDbBucket(
    std::unique_ptr<IndexedDbBackingStore> backing_store,
    std::unique_ptr<IndexedDbQuota> quota)
    : backing_store_(std::move(backing_store)),
      quota_(std::move(quota)),
      reported_size_(backing_store_->Size()) {}

IndexedDbBucket::~IndexedDbBucket() {
  ForceClose("The database is closing.");
}

void IndexedDbBucket::GetDatabaseInfo(IndexedDbReply reply) {
  PageIndexedDbCallback answer;
  answer.op = IndexedDbCallbackOp::kDatabaseInfo;
  answer.databases = backing_store_->GetDatabaseNamesAndVersions();
  std::move(reply).Run(std::move(answer));
}

void IndexedDbBucket::Open(
    std::u16string name,
    int64_t version,
    uint64_t transaction_id,
    uintptr_t client_token,
    std::unique_ptr<IndexedDbFactoryClient> client,
    std::unique_ptr<IndexedDbDatabaseCallbacks> callbacks) {
  GetOrCreateDatabase(name).ScheduleOpenConnection(
      std::move(client), std::move(callbacks), client_token, version,
      transaction_id);
}

void IndexedDbBucket::DeleteDatabase(
    std::u16string name,
    std::unique_ptr<IndexedDbFactoryClient> client) {
  GetOrCreateDatabase(name).ScheduleDeleteDatabase(std::move(client));
}

void IndexedDbBucket::QueueRunTasks() {
  if (task_run_queued_) {
    return;
  }
  task_run_queued_ = true;
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&IndexedDbBucket::RunTasks, weak_factory_.GetWeakPtr()));
}

void IndexedDbBucket::RunTasks() {
  task_run_queued_ = false;
  for (auto it = databases_.begin(); it != databases_.end();) {
    IndexedDbDatabase& database = *it->second;
    if (!database.RunTasks()) {
      // Chrome's on-disk store closes the whole bucket after an error.
      ForceClose("Internal error.");
      return;
    }
    if (database.CanBeDestroyed()) {
      it = databases_.erase(it);
    } else {
      ++it;
    }
  }
}

void IndexedDbBucket::UpdateUsage() {
  const int64_t size = backing_store_->Size();
  if (size == reported_size_) {
    return;
  }
  quota_->NotifyModified(size - reported_size_);
  reported_size_ = size;
}

void IndexedDbBucket::ForceClose(const std::string& message) {
  // Closing may queue tasks, which find nothing.
  std::map<std::u16string, std::unique_ptr<IndexedDbDatabase>> databases =
      std::move(databases_);
  databases_.clear();
  for (auto& [name, database] : databases) {
    database->ForceCloseConnectionsAndCancelRequests(message);
    CHECK(database->CanBeDestroyed());
  }
}

IndexedDbDatabase& IndexedDbBucket::GetOrCreateDatabase(
    const std::u16string& name) {
  std::unique_ptr<IndexedDbDatabase>& database = databases_[name];
  if (!database) {
    database = std::make_unique<IndexedDbDatabase>(name, *this);
  }
  return *database;
}

}  // namespace mitto
