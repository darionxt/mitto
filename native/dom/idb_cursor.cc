#include "mitto/dom/idb_cursor.h"

#include <iterator>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/notreached.h"
#include "base/task/single_thread_task_runner.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_idb_cursor.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/idb_any.h"
#include "mitto/dom/idb_database.h"
#include "mitto/dom/idb_event_dispatcher.h"
#include "mitto/dom/idb_index.h"
#include "mitto/dom/idb_key_conversion.h"
#include "mitto/dom/idb_object_store.h"
#include "mitto/dom/idb_request.h"
#include "mitto/dom/idb_transaction.h"
#include "mitto/dom/indexed_db_host.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

void CloseIDBCursor(IndexedDbHost* host, uint64_t cursor) {
  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kCloseCursor;
  call.cursor = cursor;
  host->Tell(std::move(call));
}

// static
IndexedDbCursorDirection IDBCursor::DirectionFromString(
    const std::u16string& direction) {
  if (direction == u"nextunique") {
    return IndexedDbCursorDirection::kNextNoDuplicate;
  }
  if (direction == u"prev") {
    return IndexedDbCursorDirection::kPrev;
  }
  if (direction == u"prevunique") {
    return IndexedDbCursorDirection::kPrevNoDuplicate;
  }
  DCHECK(direction == u"next");
  return IndexedDbCursorDirection::kNext;
}

IDBCursor::IDBCursor(uint64_t cursor,
                     IndexedDbCursorDirection direction,
                     IDBRequest* request,
                     IDBObjectStore* source_object_store,
                     IDBIndex* source_index,
                     IDBTransaction* transaction)
    : host_(transaction->db().Host()),
      cursor_(cursor),
      request_(request),
      direction_(direction),
      source_object_store_(source_object_store),
      source_index_(source_index),
      transaction_(transaction) {
  DCHECK(request_);
  DCHECK(!source_object_store_ != !source_index_);
  DCHECK(transaction_);
  transaction_->RegisterCursor(this);
}

IDBCursor::~IDBCursor() = default;

const bindings::WrapperTypeInfo* IDBCursor::GetWrapperTypeInfo() const {
  return &bindings::V8IDBCursor::kWrapperTypeInfo;
}

void IDBCursor::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(request_);
  visitor->Trace(saved_request_);
  visitor->Trace(source_object_store_);
  visitor->Trace(source_index_);
  visitor->Trace(transaction_);
  visitor->Trace(cached_key_);
  visitor->Trace(cached_primary_key_);
  visitor->Trace(cached_value_);
}

bool IDBCursor::IsKeyCursor() const {
  return true;
}

bool IDBCursor::IsCursorWithValue() const {
  return false;
}

void IDBCursor::ContextWillBeDestroyed() {
  ResetPrefetchCache();
}

IDBRequest* IDBCursor::Update(v8::Local<v8::Value> value,
                              bindings::ExceptionState& exception_state) {
  static constexpr char kReadOnlyUpdateErrorMessage[] =
      "The record may not be updated inside a read-only transaction.";
  if (!CheckForCommonExceptions(exception_state, kReadOnlyUpdateErrorMessage)) {
    return nullptr;
  }

  IDBObjectStore* const object_store = EffectiveObjectStore();
  return object_store->DoPut(IndexedDbPutMode::kCursorUpdate, this, value,
                             &IdbPrimaryKey(), exception_state);
}

void IDBCursor::Advance(uint32_t count,
                        bindings::ExceptionState& exception_state) {
  if (!count) {
    exception_state.ThrowTypeError(
        "A count argument with value 0 (zero) was supplied, must be greater "
        "than 0.");
    return;
  }
  if (!CheckForCommonExceptions(exception_state, nullptr)) {
    return;
  }

  request_->SetPendingCursor(this);
  got_value_ = false;

  CHECK(remote_bound_);
  AdvanceImpl(count, request_.Get());
}

void IDBCursor::Continue(v8::Local<v8::Value> key_value,
                         bindings::ExceptionState& exception_state) {
  if (!CheckForCommonExceptions(exception_state, nullptr)) {
    return;
  }

  // None for no key.
  IDBKey key;
  if (!key_value->IsUndefined() && !key_value->IsNull()) {
    if (!CreateIDBKeyFromValue(v8::Isolate::GetCurrent(), key_value,
                               exception_state, &key)) {
      return;
    }
    if (!key.IsValid()) {
      exception_state.ThrowDOMException("DataError",
                                        IDBDatabase::kNotValidKeyErrorMessage);
      return;
    }
  }
  Continue(std::move(key), IDBKey(), exception_state);
}

void IDBCursor::ContinuePrimaryKey(v8::Local<v8::Value> key_value,
                                   v8::Local<v8::Value> primary_key_value,
                                   bindings::ExceptionState& exception_state) {
  if (!transaction_->IsActive()) {
    exception_state.ThrowDOMException("TransactionInactiveError",
                                      transaction_->InactiveErrorMessage());
    return;
  }

  if (IsDeleted()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      IDBDatabase::kSourceDeletedErrorMessage);
    return;
  }

  if (!source_index_) {
    exception_state.ThrowDOMException("InvalidAccessError",
                                      "The cursor's source is not an index.");
    return;
  }

  if (direction_ != IndexedDbCursorDirection::kNext &&
      direction_ != IndexedDbCursorDirection::kPrev) {
    exception_state.ThrowDOMException(
        "InvalidAccessError",
        "The cursor's direction is not 'next' or 'prev'.");
    return;
  }

  // Some of these checks repeat those above, which keeps the order of the
  // exceptions that WPT's idbcursor-continuePrimaryKey-exception-order.html
  // tests.
  if (!CheckForCommonExceptions(exception_state, nullptr)) {
    return;
  }

  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  IDBKey key;
  if (!CreateIDBKeyFromValue(isolate, key_value, exception_state, &key)) {
    return;
  }
  if (!key.IsValid()) {
    exception_state.ThrowDOMException("DataError",
                                      IDBDatabase::kNotValidKeyErrorMessage);
    return;
  }

  IDBKey primary_key;
  if (!CreateIDBKeyFromValue(isolate, primary_key_value, exception_state,
                             &primary_key)) {
    return;
  }
  if (!primary_key.IsValid()) {
    exception_state.ThrowDOMException("DataError",
                                      IDBDatabase::kNotValidKeyErrorMessage);
    return;
  }

  Continue(std::move(key), std::move(primary_key), exception_state);
}

void IDBCursor::Continue(IDBKey key,
                         IDBKey primary_key,
                         bindings::ExceptionState& exception_state) {
  DCHECK(transaction_->IsActive());
  DCHECK(got_value_);
  DCHECK(!IsDeleted());
  DCHECK(primary_key.type() == IDBKeyType::kNone ||
         key.type() != IDBKeyType::kNone);

  const IDBKey& current_primary_key = IdbPrimaryKey();
  const bool has_primary_key = primary_key.type() != IDBKeyType::kNone;

  if (key.type() != IDBKeyType::kNone) {
    if (direction_ == IndexedDbCursorDirection::kNext ||
        direction_ == IndexedDbCursorDirection::kNextNoDuplicate) {
      const bool ok =
          key_.IsLessThan(key) || (has_primary_key && key_.Equals(key) &&
                                   current_primary_key.IsLessThan(primary_key));
      if (!ok) {
        exception_state.ThrowDOMException(
            "DataError",
            "The parameter is less than or equal to this cursor's position.");
        return;
      }
    } else {
      const bool ok =
          key.IsLessThan(key_) || (has_primary_key && key.Equals(key_) &&
                                   primary_key.IsLessThan(current_primary_key));
      if (!ok) {
        exception_state.ThrowDOMException(
            "DataError",
            "The parameter is greater than or equal to this cursor's "
            "position.");
        return;
      }
    }
  }

  // Blink notes that the answer comes in the context that opened the
  // cursor, not the one continue() was called in.
  request_->SetPendingCursor(this);
  got_value_ = false;
  CHECK(remote_bound_);
  CursorContinue(key, primary_key, request_.Get());
}

IDBRequest* IDBCursor::Delete(bindings::ExceptionState& exception_state) {
  static constexpr char kReadOnlyDeleteErrorMessage[] =
      "The record may not be deleted inside a read-only transaction.";
  if (!CheckForCommonExceptions(exception_state, kReadOnlyDeleteErrorMessage)) {
    return nullptr;
  }

  IDBRequest* const request = IDBRequest::Create(this, transaction_.Get());
  transaction_->db().Delete(transaction_->Id(), EffectiveObjectStore()->Id(),
                            IdbPrimaryKey(), request);
  return request;
}

void IDBCursor::Close() {
  value_.reset();
  request_.Clear();
  if (remote_bound_) {
    remote_bound_ = false;
    CloseIDBCursor(host_, cursor_);
  }
  ResetPrefetchCache();
  transaction_->UnregisterCursor(this);
}

v8::Local<v8::Value> IDBCursor::Key() {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (!key_dirty_ && !cached_key_.IsEmpty()) {
    return cached_key_.Get(isolate);
  }
  key_dirty_ = false;
  v8::Local<v8::Value> key = IDBKeyToV8(isolate, key_);
  if (key.IsEmpty()) {
    key = v8::Undefined(isolate);
  }
  cached_key_.Reset(isolate, key);
  return key;
}

v8::Local<v8::Value> IDBCursor::PrimaryKey() {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (!primary_key_dirty_ && !cached_primary_key_.IsEmpty()) {
    return cached_primary_key_.Get(isolate);
  }
  primary_key_dirty_ = false;
  // Blink takes it from the value when it put it there, which is the same
  // key.
  v8::Local<v8::Value> primary_key = IDBKeyToV8(isolate, primary_key_);
  if (primary_key.IsEmpty()) {
    primary_key = v8::Undefined(isolate);
  }
  cached_primary_key_.Reset(isolate, primary_key);
  return primary_key;
}

v8::Local<v8::Value> IDBCursor::Value() {
  DCHECK(IsCursorWithValue());
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (!value_dirty_ && !cached_value_.IsEmpty()) {
    return cached_value_.Get(isolate);
  }
  value_dirty_ = false;
  // Undefined for none, as Blink's empty IDBAny gives.
  v8::Local<v8::Value> value;
  if (value_) {
    value = DeserializeIDBValue(isolate, *value_);
  }
  if (value.IsEmpty()) {
    value = value_ ? v8::Null(isolate).As<v8::Value>()
                   : v8::Undefined(isolate).As<v8::Value>();
  }
  cached_value_.Reset(isolate, value);
  return value;
}

IDBRequest* IDBCursor::Request() {
  if (!request_saved_) {
    request_saved_ = true;
    saved_request_ = request_;
  }
  return saved_request_.Get();
}

bindings::ScriptWrappable* IDBCursor::Source() const {
  if (source_index_) {
    return source_index_.Get();
  }
  return source_object_store_.Get();
}

void IDBCursor::SetValueReady(IDBKey key,
                              IDBKey primary_key,
                              IndexedDbReturnValue value) {
  key_ = std::move(key);
  key_dirty_ = true;

  primary_key_ = std::move(primary_key);
  primary_key_dirty_ = true;

  got_value_ = true;

  if (!IsCursorWithValue()) {
    return;
  }

  value_dirty_ = true;

  // A key that the store's key generator made goes into the value at its key
  // path, which the Session's value lacks.
  IDBObjectStore* const object_store = EffectiveObjectStore();
  if (object_store->AutoIncrement() &&
      !object_store->Metadata().key_path.IsNull()) {
    value.primary_key = primary_key_;
    value.key_path = object_store->Metadata().key_path;
  }

  value_ = std::move(value);
}

IDBObjectStore* IDBCursor::EffectiveObjectStore() const {
  if (source_index_) {
    return source_index_->ObjectStore();
  }
  return source_object_store_.Get();
}

bool IDBCursor::IsDeleted() const {
  if (source_index_) {
    return source_index_->IsDeleted();
  }
  return source_object_store_->IsDeleted();
}

std::u16string IDBCursor::Direction() const {
  switch (direction_) {
    case IndexedDbCursorDirection::kNext:
      return u"next";
    case IndexedDbCursorDirection::kNextNoDuplicate:
      return u"nextunique";
    case IndexedDbCursorDirection::kPrev:
      return u"prev";
    case IndexedDbCursorDirection::kPrevNoDuplicate:
      return u"prevunique";
  }
  NOTREACHED();
}

void IDBCursor::AdvanceImpl(uint32_t count, IDBRequest* request) {
  if (count <= prefetch_keys_.size()) {
    CachedAdvance(count, request);
    return;
  }
  ResetPrefetchCache();

  // Resets the prefetch caches of the transaction's other cursors.
  transaction_->ResetCursorPrefetchCaches(this);

  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kCursorAdvance;
  call.count = count;
  Ask(std::move(call), request, AnswerTo::kAdvanceCallback);
}

void IDBCursor::AdvanceCallback(IDBRequest* request,
                                PageIndexedDbCallback answer) {
  if (request) {
    request->OnAdvanceCursor(answer);
  }
}

void IDBCursor::CursorContinue(const IDBKey& key,
                               const IDBKey& primary_key,
                               IDBRequest* request) {
  if (key.type() == IDBKeyType::kNone &&
      primary_key.type() == IDBKeyType::kNone) {
    // No key, so this could be prefetched.
    ++continue_count_;

    if (!prefetch_keys_.empty()) {
      // The prefetched records serve it.
      CachedContinue(request);
      return;
    }

    if (continue_count_ > kPrefetchContinueThreshold) {
      ++pending_onsuccess_callbacks_;

      PageIndexedDbCall call;
      call.op = IndexedDbCallOp::kCursorPrefetch;
      call.count = static_cast<uint32_t>(prefetch_amount_);
      Ask(std::move(call), request, AnswerTo::kPrefetchCallback);

      // Twice as many next time, up to the most.
      prefetch_amount_ *= 2;
      if (prefetch_amount_ > kMaxPrefetchAmount) {
        prefetch_amount_ = kMaxPrefetchAmount;
      }
      return;
    }
  } else {
    // A key cannot be prefetched.
    ResetPrefetchCache();
  }

  // Resets the prefetch caches of the transaction's other cursors.
  transaction_->ResetCursorPrefetchCaches(this);

  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kCursorContinue;
  call.key = key;
  call.primary_key = primary_key;
  Ask(std::move(call), request, AnswerTo::kAdvanceCallback);
}

void IDBCursor::PrefetchCallback(IDBRequest* request,
                                 PageIndexedDbCallback answer) {
  // A key cursor's records come without values, where Mojo's carry a null
  // one for each.
  if (answer.error == IndexedDbException::kNoError && !answer.empty &&
      !answer.keys.empty() &&
      answer.keys.size() == answer.primary_keys.size() &&
      (answer.values.empty() || answer.values.size() == answer.keys.size())) {
    answer.values.resize(answer.keys.size());
    SetPrefetchData(std::move(answer.keys), std::move(answer.primary_keys),
                    std::move(answer.values));
    CachedContinue(request);
  } else if (request) {
    // An error goes as an advance()'s does.
    request->OnAdvanceCursor(answer);
  }
}

void IDBCursor::PostSuccessHandlerCallback() {
  --pending_onsuccess_callbacks_;

  // A listener that called continue() or advance() again, which the
  // prefetched records served, counted one more; otherwise it did something
  // else, or nothing, and they go.
  if (pending_onsuccess_callbacks_ == 0) {
    ResetPrefetchCache();
  }
}

void IDBCursor::SetPrefetchData(std::vector<IDBKey> keys,
                                std::vector<IDBKey> primary_keys,
                                std::vector<IndexedDbReturnValue> values) {
  // Last first, so that a continue() takes the one at the back.
  prefetch_keys_.insert(prefetch_keys_.end(),
                        std::make_move_iterator(keys.rbegin()),
                        std::make_move_iterator(keys.rend()));
  prefetch_primary_keys_.insert(prefetch_primary_keys_.end(),
                                std::make_move_iterator(primary_keys.rbegin()),
                                std::make_move_iterator(primary_keys.rend()));
  prefetch_values_.insert(prefetch_values_.end(),
                          std::make_move_iterator(values.rbegin()),
                          std::make_move_iterator(values.rend()));

  used_prefetches_ = 0;
  pending_onsuccess_callbacks_ = 0;
}

void IDBCursor::CachedAdvance(uint32_t count, IDBRequest* request) {
  DCHECK_GE(prefetch_keys_.size(), count);
  DCHECK_EQ(prefetch_primary_keys_.size(), prefetch_keys_.size());
  DCHECK_EQ(prefetch_values_.size(), prefetch_keys_.size());

  while (count > 1) {
    prefetch_keys_.pop_back();
    prefetch_primary_keys_.pop_back();
    prefetch_values_.pop_back();
    ++used_prefetches_;
    --count;
  }

  CachedContinue(request);
}

void IDBCursor::CachedContinue(IDBRequest* request) {
  CHECK(!prefetch_keys_.empty());
  CHECK_EQ(prefetch_primary_keys_.size(), prefetch_keys_.size());
  CHECK_EQ(prefetch_values_.size(), prefetch_keys_.size());

  IDBKey key = std::move(prefetch_keys_.back());
  IDBKey primary_key = std::move(prefetch_primary_keys_.back());
  IndexedDbReturnValue value = std::move(prefetch_values_.back());

  prefetch_keys_.pop_back();
  prefetch_primary_keys_.pop_back();
  prefetch_values_.pop_back();
  ++used_prefetches_;

  ++pending_onsuccess_callbacks_;

  if (!continue_count_) {
    // ResetPrefetchCache() ran after the prefetch was asked for: the
    // continue() that asked for it has its record now, and the rest go.
    ResetPrefetchCache();
  }

  if (request) {
    // A record that does not come from the Session joins the transaction's
    // results from a task of its own, as the spec's continue() queues it. A
    // result of the Session's that came before would have reset this cache
    // first.
    PostIDBTask(request->GetWindow(),
                base::BindOnce(
                    [](cppgc::WeakPersistent<IDBRequest> request, IDBKey key,
                       IDBKey primary_key, IndexedDbReturnValue value) {
                      if (request) {
                        request->HandleResponseAdvanceCursor(
                            std::move(key), std::move(primary_key),
                            std::move(value));
                      }
                    },
                    cppgc::WeakPersistent<IDBRequest>(request), std::move(key),
                    std::move(primary_key), std::move(value)));
  }
}

void IDBCursor::ResetPrefetchCache() {
  continue_count_ = 0;
  prefetch_amount_ = kMinPrefetchAmount;

  if (prefetch_keys_.empty()) {
    // Nothing was prefetched, so the Session's cursor stays where it is.
    return;
  }

  // The Session's cursor goes back to after the last record that was used.
  if (remote_bound_) {
    PageIndexedDbCall call;
    call.op = IndexedDbCallOp::kCursorPrefetchReset;
    call.count = static_cast<uint32_t>(used_prefetches_);
    Tell(std::move(call));
  }

  prefetch_keys_.clear();
  prefetch_primary_keys_.clear();
  prefetch_values_.clear();

  pending_onsuccess_callbacks_ = 0;
}

bool IDBCursor::CheckForCommonExceptions(
    bindings::ExceptionState& exception_state,
    const char* read_only_error_message) {
  if (!transaction_->IsActive()) {
    exception_state.ThrowDOMException("TransactionInactiveError",
                                      transaction_->InactiveErrorMessage());
    return false;
  }
  if (read_only_error_message && transaction_->IsReadOnly()) {
    exception_state.ThrowDOMException("ReadOnlyError", read_only_error_message);
    return false;
  }
  if (IsDeleted()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      IDBDatabase::kSourceDeletedErrorMessage);
    return false;
  }
  if (!got_value_) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      IDBDatabase::kNoValueErrorMessage);
    return false;
  }
  if (read_only_error_message && IsKeyCursor()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      IDBDatabase::kIsKeyCursorErrorMessage);
    return false;
  }
  if (!request_ || !request_->HasContext()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      IDBDatabase::kDatabaseClosedErrorMessage);
    return false;
  }
  return true;
}

void IDBCursor::Ask(PageIndexedDbCall call,
                    IDBRequest* request,
                    AnswerTo answer_to) {
  if (!transaction_->db().IsConnectionOpen()) {
    return;
  }
  call.cursor = cursor_;
  host_->Ask(std::move(call),
             base::BindRepeating(
                 [](cppgc::Persistent<IDBCursor> cursor,
                    cppgc::WeakPersistent<IDBRequest> request,
                    AnswerTo answer_to, PageIndexedDbCallback answer) {
                   RunInIDBContext(cursor->transaction_->GetWindow(), [&] {
                     cursor->OnAnswer(request.Get(), answer_to,
                                      std::move(answer));
                   });
                 },
                 cppgc::Persistent<IDBCursor>(this),
                 cppgc::WeakPersistent<IDBRequest>(request), answer_to));
}

void IDBCursor::Tell(PageIndexedDbCall call) {
  if (!transaction_->db().IsConnectionOpen()) {
    return;
  }
  call.cursor = cursor_;
  host_->Tell(std::move(call));
}

void IDBCursor::OnAnswer(IDBRequest* request,
                         AnswerTo answer_to,
                         PageIndexedDbCallback answer) {
  switch (answer_to) {
    case AnswerTo::kAdvanceCallback:
      AdvanceCallback(request, std::move(answer));
      return;
    case AnswerTo::kPrefetchCallback:
      PrefetchCallback(request, std::move(answer));
      return;
  }
  NOTREACHED();
}

void IDBCursor::Dispose() {
  if (!remote_bound_) {
    return;
  }
  remote_bound_ = false;
  // Garbage collection may not send, so the cursor closes from a task.
  EventLoop::Get()
      ->TaskRunner(TaskType::kDatabaseAccess)
      ->PostTask(FROM_HERE,
                 base::BindOnce(&CloseIDBCursor, host_.get(), cursor_));
}

}  // namespace mitto
