#include "mitto/dom/idb_request_queue_item.h"

#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/dom_exception.h"
#include "mitto/dom/idb_cursor.h"
#include "mitto/dom/idb_database.h"
#include "mitto/dom/idb_request.h"
#include "mitto/dom/idb_transaction.h"

namespace mitto {

IDBRequestQueueItem::IDBRequestQueueItem(IDBRequest* request,
                                         DOMException* error,
                                         base::OnceClosure on_result_ready)
    : request_(request),
      error_(error),
      on_result_ready_(std::move(on_result_ready)),
      response_type_(kError) {
  CHECK(on_result_ready_);
  CHECK_EQ(request->queue_item_, nullptr);
  request_->queue_item_ = this;
}

IDBRequestQueueItem::IDBRequestQueueItem(IDBRequest* request,
                                         int64_t value,
                                         base::OnceClosure on_result_ready)
    : request_(request),
      on_result_ready_(std::move(on_result_ready)),
      int64_value_(value),
      response_type_(kNumber) {
  CHECK(on_result_ready_);
  CHECK_EQ(request->queue_item_, nullptr);
  request_->queue_item_ = this;
}

IDBRequestQueueItem::IDBRequestQueueItem(IDBRequest* request,
                                         base::OnceClosure on_result_ready)
    : request_(request),
      on_result_ready_(std::move(on_result_ready)),
      response_type_(kVoid) {
  CHECK(on_result_ready_);
  CHECK_EQ(request->queue_item_, nullptr);
  request_->queue_item_ = this;
}

IDBRequestQueueItem::IDBRequestQueueItem(IDBRequest* request,
                                         IDBKey key,
                                         base::OnceClosure on_result_ready)
    : request_(request),
      key_(std::move(key)),
      on_result_ready_(std::move(on_result_ready)),
      response_type_(kKey) {
  CHECK(on_result_ready_);
  CHECK_EQ(request->queue_item_, nullptr);
  request_->queue_item_ = this;
}

IDBRequestQueueItem::IDBRequestQueueItem(IDBRequest* request,
                                         IndexedDbReturnValue value,
                                         base::OnceClosure on_result_ready)
    : request_(request),
      on_result_ready_(std::move(on_result_ready)),
      response_type_(kValue) {
  CHECK(on_result_ready_);
  CHECK_EQ(request->queue_item_, nullptr);
  request_->queue_item_ = this;
  // A value over 64 KiB that Chrome wrapped in a Blob would wait here for
  // IDBRequestLoader; the Session's values come whole.
  records_.values.push_back(std::move(value));
}

IDBRequestQueueItem::IDBRequestQueueItem(IDBRequest* request,
                                         IDBKey key,
                                         IDBKey primary_key,
                                         IndexedDbReturnValue value,
                                         base::OnceClosure on_result_ready)
    : request_(request),
      key_(std::move(key)),
      primary_key_(std::move(primary_key)),
      on_result_ready_(std::move(on_result_ready)),
      response_type_(kKeyPrimaryKeyValue) {
  CHECK(on_result_ready_);
  CHECK_EQ(request->queue_item_, nullptr);
  request_->queue_item_ = this;
  records_.values.push_back(std::move(value));
}

IDBRequestQueueItem::IDBRequestQueueItem(IDBRequest* request,
                                         uint64_t cursor,
                                         IDBKey key,
                                         IDBKey primary_key,
                                         IndexedDbReturnValue value,
                                         base::OnceClosure on_result_ready)
    : request_(request),
      key_(std::move(key)),
      primary_key_(std::move(primary_key)),
      on_result_ready_(std::move(on_result_ready)),
      pending_cursor_(cursor),
      response_type_(kCursorKeyPrimaryKeyValue) {
  CHECK(on_result_ready_);
  CHECK_EQ(request->queue_item_, nullptr);
  request_->queue_item_ = this;
  records_.values.push_back(std::move(value));
}

IDBRequestQueueItem::IDBRequestQueueItem(
    IDBRequest* request,
    IndexedDbGetAllResultType get_all_result_type,
    base::OnceClosure on_result_ready)
    : request_(request),
      on_result_ready_(std::move(on_result_ready)),
      get_all_active_(true),
      get_all_result_type_(get_all_result_type) {
  CHECK(on_result_ready_);
  CHECK_EQ(request->queue_item_, nullptr);
  request_->queue_item_ = this;
}

IDBRequestQueueItem::~IDBRequestQueueItem() = default;

void IDBRequestQueueItem::ReceiveResults(
    const std::vector<IndexedDbRecord>& results,
    bool done) {
  CHECK(get_all_active_);
  const bool first_chunk = !get_all_received_chunk_;
  get_all_received_chunk_ = true;

  // Each record's primary key, value and index key, of which the kind of
  // result that was asked for leaves some out.
  for (const IndexedDbRecord& result : results) {
    if (result.primary_key) {
      get_all_records_.primary_keys.push_back(*result.primary_key);
    }
    if (result.value) {
      get_all_records_.values.push_back(*result.value);
    }
    if (result.index_key) {
      get_all_records_.index_keys.push_back(*result.index_key);
    }
  }
  if (!done) {
    return;
  }

  get_all_active_ = false;
  switch (get_all_result_type_) {
    case IndexedDbGetAllResultType::kKeys:
      response_type_ = kKeyArray;
      break;
    case IndexedDbGetAllResultType::kValues:
      response_type_ = kValueArray;
      break;
    case IndexedDbGetAllResultType::kRecords:
      response_type_ = kRecordArray;
      break;
  }
  records_ = std::move(get_all_records_);

  // All of it came with the call, before the item was queued: StartLoading()
  // makes it ready.
  if (first_chunk) {
    return;
  }
  OnResultReady();
}

void IDBRequestQueueItem::OnGetAllError(IndexedDbException code,
                                        const std::u16string& message) {
  CHECK(get_all_active_);
  response_type_ = kError;
  error_ = MakeIDBException(code, message);
  get_all_active_ = false;
  OnResultReady();
}

void IDBRequestQueueItem::OnResultReady() {
  CHECK(!ready_);
  ready_ = true;

  CHECK(on_result_ready_);
  std::move(on_result_ready_).Run();
}

void IDBRequestQueueItem::StartLoading() {
  started_loading_ = true;

  // A getAll() that waits for its records is ready once its last chunk
  // comes.
  if (get_all_active_) {
    return;
  }

  if (request_->request_aborted_) {
    // The Session can answer a request after its transaction aborted it, and
    // IDBRequest::Abort() did not cancel this, which was not queued yet.
    CancelLoading();
    return;
  }

  OnResultReady();
}

void IDBRequestQueueItem::CancelLoading() {
  if (ready_) {
    return;
  }

  // The rest of a getAll()'s records go nowhere.
  get_all_active_ = false;
  get_all_records_.clear();

  // Ready, so that the transaction's queue can be drained.
  response_type_ = kCanceled;
  records_.clear();

  OnResultReady();
}

void IDBRequestQueueItem::SendResult() {
  CHECK(ready_);
  CHECK_EQ(request_->queue_item_, this);
  request_->queue_item_ = nullptr;

  IDBRequest* const request = request_.Get();
  switch (response_type_) {
    case kCanceled:
      // Blink drops the cursor's pending remote with the item, which closes
      // the Session's cursor.
      if (pending_cursor_ && request->Transaction()) {
        CloseIDBCursor(request->Transaction()->db().Host(), pending_cursor_);
      }
      break;
    case kCursorKeyPrimaryKeyValue:
      CHECK_EQ(records_.values.size(), 1u);
      request->SendResultCursor(std::exchange(pending_cursor_, 0),
                                std::move(key_), std::move(primary_key_),
                                std::move(records_.values.front()));
      break;
    case kKeyPrimaryKeyValue:
      CHECK_EQ(records_.values.size(), 1u);
      request->SendResultAdvanceCursor(std::move(key_), std::move(primary_key_),
                                       std::move(records_.values.front()));
      break;
    case kError:
      CHECK(error_);
      request->SendError(error_.Get());
      break;
    case kKey:
      if (key_.IsValid()) {
        request->SendResult(
            bindings::MakeGarbageCollected<IDBAny>(std::move(key_)));
      } else {
        request->SendResult(bindings::MakeGarbageCollected<IDBAny>());
      }
      break;
    case kKeyArray:
      CHECK(records_.values.empty());
      CHECK(records_.index_keys.empty());
      request->SendResult(bindings::MakeGarbageCollected<IDBAny>(
          IDBKey::Array(std::move(records_.primary_keys))));
      break;
    case kNumber:
      request->SendResult(bindings::MakeGarbageCollected<IDBAny>(int64_value_));
      break;
    case kRecordArray:
      // A primary key and a value for each record, and an index key too for
      // an index's.
      CHECK_EQ(records_.primary_keys.size(), records_.values.size());
      CHECK(records_.index_keys.empty() ||
            records_.index_keys.size() == records_.primary_keys.size());
      request->SendResult(
          bindings::MakeGarbageCollected<IDBAny>(std::move(records_)));
      break;
    case kValue:
      CHECK_EQ(records_.values.size(), 1u);
      request->SendResultValue(std::move(records_.values.front()));
      break;
    case kValueArray:
      CHECK(records_.primary_keys.empty());
      CHECK(records_.index_keys.empty());
      request->SendResult(
          bindings::MakeGarbageCollected<IDBAny>(std::move(records_.values)));
      break;
    case kVoid:
      request->SendResult(bindings::MakeGarbageCollected<IDBAny>());
      break;
  }
}

}  // namespace mitto
