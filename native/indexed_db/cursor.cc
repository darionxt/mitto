#include <utility>

#include "base/functional/bind.h"
#include "mitto/indexed_db/engine.h"

namespace mitto {
namespace {

// What one prefetch sends, as Chrome's estimate counts it, past which it
// stops.
constexpr size_t kMaxPrefetchSizeEstimate = 10 * 1024 * 1024;

// What one answer holds at most. Chrome's values are at most 64 KiB, for it
// wraps bigger ones into blobs; the values here are whole, so an entry that
// would take the answer past what a frame holds waits for the next one.
constexpr size_t kMaxAnswerSize = 12 * 1024 * 1024;

PageIndexedDbCallback CursorResult() {
  PageIndexedDbCallback answer;
  answer.op = IndexedDbCallbackOp::kCursorResult;
  return answer;
}

PageIndexedDbCallback CursorEnded() {
  PageIndexedDbCallback answer = CursorResult();
  answer.empty = true;
  return answer;
}

size_t EntrySize(const IndexedDbBackingStore::Cursor& cursor) {
  return cursor.key().SizeEstimate() + cursor.primary_key().SizeEstimate() +
         cursor.value().size();
}

}  // namespace

IndexedDbCursor::IndexedDbCursor(
    std::unique_ptr<IndexedDbBackingStore::Cursor> cursor,
    bool is_index,
    IndexedDbCursorDirection direction,
    bool key_only,
    IndexedDbTaskType task_type,
    base::WeakPtr<IndexedDbTransaction> transaction)
    : cursor_(std::move(cursor)),
      is_index_(is_index),
      direction_(direction),
      key_only_(key_only),
      task_type_(task_type),
      transaction_(std::move(transaction)) {
  if (transaction_) {
    transaction_->RegisterOpenCursor(this);
  }
}

IndexedDbCursor::~IndexedDbCursor() {
  Close();
}

void IndexedDbCursor::Close() {
  if (closed_) {
    return;
  }
  closed_ = true;
  weak_factory_.InvalidateWeakPtrs();
  cursor_.reset();
  if (transaction_) {
    transaction_->UnregisterOpenCursor(this);
  }
  transaction_.reset();
}

bool IndexedDbCursor::CheckOpen(IndexedDbReply& reply) {
  if (!transaction_ || !transaction_->IsAcceptingRequests()) {
    Close();
  }
  if (!closed_) {
    return true;
  }
  PageIndexedDbCallback answer = CursorResult();
  answer.error = IndexedDbException::kUnknownError;
  answer.message = u"The cursor has been closed.";
  std::move(reply).Run(std::move(answer));
  return false;
}

PageIndexedDbCallback IndexedDbCursor::CurrentEntry() {
  PageIndexedDbCallback answer = CursorResult();
  answer.keys.push_back(cursor_->key());
  answer.primary_keys.push_back(cursor_->primary_key());
  if (!key_only_) {
    IndexedDbReturnValue value;
    value.bits = cursor_->value();
    answer.values.push_back(std::move(value));
  }
  return answer;
}

bool IndexedDbCursor::Advance(uint32_t count, IndexedDbReply reply) {
  if (count == 0) {
    return false;
  }
  if (!CheckOpen(reply)) {
    return true;
  }
  transaction_->ScheduleTask(
      task_type_,
      BindWeakOperation(
          &IndexedDbCursor::AdvanceOperation, weak_factory_.GetWeakPtr(), count,
          AbortOnDestruct(std::move(reply), IndexedDbCallbackOp::kCursorResult,
                          transaction_)));
  return true;
}

bool IndexedDbCursor::AdvanceOperation(uint32_t count,
                                       IndexedDbReply reply,
                                       IndexedDbTransaction& transaction) {
  if (!cursor_ || !cursor_->Advance(count)) {
    cursor_.reset();
    std::move(reply).Run(CursorEnded());
    return true;
  }
  std::move(reply).Run(CurrentEntry());
  return true;
}

bool IndexedDbCursor::Continue(IDBKey key,
                               IDBKey primary_key,
                               IndexedDbReply reply) {
  // A primary key goes only with an index cursor that is not unique.
  if ((!is_index_ || direction_ == IndexedDbCursorDirection::kNextNoDuplicate ||
       direction_ == IndexedDbCursorDirection::kPrevNoDuplicate) &&
      primary_key.IsValid()) {
    return false;
  }
  if (!CheckOpen(reply)) {
    return true;
  }
  transaction_->ScheduleTask(
      task_type_,
      BindWeakOperation(
          &IndexedDbCursor::ContinueOperation, weak_factory_.GetWeakPtr(),
          std::move(key), std::move(primary_key),
          AbortOnDestruct(std::move(reply), IndexedDbCallbackOp::kCursorResult,
                          transaction_)));
  return true;
}

bool IndexedDbCursor::ContinueOperation(IDBKey key,
                                        IDBKey primary_key,
                                        IndexedDbReply reply,
                                        IndexedDbTransaction& transaction) {
  if (!cursor_ || !cursor_->Continue(key, primary_key)) {
    cursor_.reset();
    std::move(reply).Run(CursorEnded());
    return true;
  }
  std::move(reply).Run(CurrentEntry());
  return true;
}

void IndexedDbCursor::Prefetch(uint32_t count, IndexedDbReply reply) {
  if (!CheckOpen(reply)) {
    return;
  }
  transaction_->ScheduleTask(
      task_type_,
      BindWeakOperation(
          &IndexedDbCursor::PrefetchOperation, weak_factory_.GetWeakPtr(),
          count,
          AbortOnDestruct(std::move(reply), IndexedDbCallbackOp::kCursorResult,
                          transaction_)));
}

bool IndexedDbCursor::PrefetchOperation(uint32_t count,
                                        IndexedDbReply reply,
                                        IndexedDbTransaction& transaction) {
  PageIndexedDbCallback answer = CursorResult();
  size_t size_estimate = 0;
  size_t answer_size = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (!cursor_ || reached_end_during_prefetch_) {
      break;
    }
    const IndexedDbBackingStore::Cursor::Position before =
        cursor_->GetPosition();
    if (!cursor_->Continue()) {
      reached_end_during_prefetch_ = true;
      break;
    }
    if (i != 0 && answer_size + EntrySize(*cursor_) > kMaxAnswerSize) {
      cursor_->SetPosition(before);
      break;
    }
    answer_size += EntrySize(*cursor_);
    if (i == 0) {
      // The first entry is always used: a reset goes back to it.
      saved_position_ = cursor_->GetPosition();
    }
    answer.keys.push_back(cursor_->key());
    answer.primary_keys.push_back(cursor_->primary_key());
    if (!key_only_) {
      IndexedDbReturnValue value;
      value.bits = cursor_->value();
      size_estimate += value.bits.size();
      answer.values.push_back(std::move(value));
    }
    size_estimate +=
        cursor_->key().SizeEstimate() + cursor_->primary_key().SizeEstimate();
    if (size_estimate > kMaxPrefetchSizeEstimate) {
      break;
    }
  }
  if (answer.keys.empty()) {
    std::move(reply).Run(CursorEnded());
    return true;
  }
  std::move(reply).Run(std::move(answer));
  return true;
}

bool IndexedDbCursor::PrefetchReset(uint32_t used_prefetches) {
  if (closed_ || !cursor_ || !transaction_) {
    return true;
  }
  // The first prefetched entry is always used.
  if (used_prefetches == 0 || !saved_position_) {
    cursor_.reset();
    return false;
  }
  reached_end_during_prefetch_ = false;
  cursor_->SetPosition(*saved_position_);
  if (used_prefetches == 1) {
    return true;
  }
  if (!cursor_->Advance(used_prefetches - 1)) {
    cursor_.reset();
    return false;
  }
  return true;
}

}  // namespace mitto
