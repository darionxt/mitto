#include "mitto/dom/idb_request.h"

#include <memory>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "base/functional/bind.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_idb_cursor.h"
#include "mitto/bindings/v8_idb_cursor_with_value.h"
#include "mitto/bindings/v8_idb_index.h"
#include "mitto/bindings/v8_idb_object_store.h"
#include "mitto/bindings/v8_idb_request.h"
#include "mitto/dom/dom_exception.h"
#include "mitto/dom/event.h"
#include "mitto/dom/idb_any.h"
#include "mitto/dom/idb_cursor.h"
#include "mitto/dom/idb_cursor_with_value.h"
#include "mitto/dom/idb_database.h"
#include "mitto/dom/idb_event_dispatcher.h"
#include "mitto/dom/idb_index.h"
#include "mitto/dom/idb_object_store.h"
#include "mitto/dom/idb_request_queue_item.h"
#include "mitto/dom/idb_transaction.h"
#include "mitto/dom/quota_exceeded_error.h"
#include "mitto/dom/window.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

// The cursor that `source`, a request's, is, or null: Blink's
// Source::IsIDBCursor().
IDBCursor* AsCursor(bindings::ScriptWrappable* source) {
  if (!source) {
    return nullptr;
  }
  const bindings::WrapperTypeInfo* const type = source->GetWrapperTypeInfo();
  if (type == &bindings::V8IDBCursor::kWrapperTypeInfo ||
      type == &bindings::V8IDBCursorWithValue::kWrapperTypeInfo) {
    return static_cast<IDBCursor*>(source);
  }
  return nullptr;
}

// What tells a request's transaction that the request's result is ready.
base::OnceClosure OnResultReadyOf(IDBTransaction* transaction) {
  return base::BindOnce(
      [](cppgc::Persistent<IDBTransaction> transaction) {
        transaction->OnResultReady();
      },
      cppgc::Persistent<IDBTransaction>(transaction));
}

}  // namespace

DOMException* MakeIDBException(IndexedDbException code,
                               const std::u16string& message) {
  // blink.mojom.IDBException's values are DOMExceptionCode's.
  const char16_t* name = u"UnknownError";
  switch (code) {
    case IndexedDbException::kQuotaError: {
      v8::Isolate* const isolate = v8::Isolate::GetCurrent();
      if (bindings::PerIsolateData::From(isolate)->features().IsEnabled(
              bindings::RuntimeFeature::kQuotaExceededErrorUpdate)) {
        return QuotaExceededError::Create(message, std::nullopt, std::nullopt);
      }
      name = u"QuotaExceededError";
      break;
    }
    case IndexedDbException::kIgnorableAbortError:
      // DOMExceptionCode 11.
      name = u"InvalidStateError";
      break;
    case IndexedDbException::kAbortError:
      name = u"AbortError";
      break;
    case IndexedDbException::kTimeoutError:
      name = u"TimeoutError";
      break;
    case IndexedDbException::kConstraintError:
      name = u"ConstraintError";
      break;
    case IndexedDbException::kDataError:
      name = u"DataError";
      break;
    case IndexedDbException::kVersionError:
      name = u"VersionError";
      break;
    case IndexedDbException::kNoError:
    case IndexedDbException::kUnknownError:
      break;
  }
  return DOMException::Create(message, name);
}

// static
IDBRequest* IDBRequest::Create(bindings::ScriptWrappable* source,
                               IDBTransaction* transaction) {
  CHECK(transaction);
  IDBRequest* const request = bindings::MakeGarbageCollected<IDBRequest>(
      transaction->GetWindow(), source, transaction);
  transaction->RegisterRequest(request);
  return request;
}

IDBRequest::IDBRequest(Window* window,
                       bindings::ScriptWrappable* source,
                       IDBTransaction* transaction)
    : transaction_(transaction), window_(window), source_(source) {}

IDBRequest::~IDBRequest() = default;

const bindings::WrapperTypeInfo* IDBRequest::GetWrapperTypeInfo() const {
  return &bindings::V8IDBRequest::kWrapperTypeInfo;
}

void IDBRequest::Trace(cppgc::Visitor* visitor) const {
  EventTarget::Trace(visitor);
  visitor->Trace(transaction_);
  visitor->Trace(window_);
  visitor->Trace(source_);
  visitor->Trace(result_);
  visitor->Trace(error_);
  visitor->Trace(cached_result_);
  visitor->Trace(native_success_listener_);
  visitor->Trace(pending_cursor_);
}

void IDBRequest::SetNativeSuccessListener(NativeSuccessListener* listener) {
  native_success_listener_ = listener;
}

bool IDBRequest::HasContext() const {
  Window* const window = window_.Get();
  return window && window->HasContext();
}

v8::Local<v8::Value> IDBRequest::Result(
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  // [CachedAttribute=isResultDirty]: the bindings give what they kept until
  // the result changes, before asking the request.
  if (!result_dirty_ && !cached_result_.IsEmpty()) {
    return cached_result_.Get(isolate);
  }
  if (ready_state_ != kDone) {
    exception_state.ThrowDOMException(
        "InvalidStateError", IDBDatabase::kRequestNotFinishedErrorMessage);
    return {};
  }
  if (!HasContext()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      IDBDatabase::kDatabaseClosedErrorMessage);
    return {};
  }
  result_dirty_ = false;
  v8::Local<v8::Value> value =
      result_ ? result_->ToV8(isolate) : v8::Null(isolate).As<v8::Value>();
  if (value.IsEmpty()) {
    value = v8::Undefined(isolate);
  }
  cached_result_.Reset(isolate, value);
  return value;
}

DOMException* IDBRequest::Error(
    bindings::ExceptionState& exception_state) const {
  if (ready_state_ != kDone) {
    exception_state.ThrowDOMException(
        "InvalidStateError", IDBDatabase::kRequestNotFinishedErrorMessage);
    return nullptr;
  }
  return error_.Get();
}

bindings::ScriptWrappable* IDBRequest::Source() const {
  if (!HasContext()) {
    return nullptr;
  }
  return source_.Get();
}

std::u16string IDBRequest::ReadyState() const {
  if (!HasContext()) {
    return u"done";
  }
  return ready_state_ == kPending ? u"pending" : u"done";
}

void IDBRequest::Abort(bool queue_dispatch) {
  DCHECK(!request_aborted_);
  if (queue_item_) {
    queue_item_->CancelLoading();
  }

  if (!HasContext()) {
    return;
  }
  if (ready_state_ == kDone) {
    return;
  }

  request_aborted_ = true;
  DOMException* const error = DOMException::Create(
      u"The transaction was aborted, so the request cannot be fulfilled.",
      u"AbortError");
  if (queue_dispatch) {
    // Held until its error fires, as Blink's pending activity keeps a request
    // that is not done, which its transaction no longer holds.
    PostIDBTask(window_.Get(), base::BindOnce(
                                   [](cppgc::Persistent<IDBRequest> request,
                                      cppgc::Persistent<DOMException> error) {
                                     request->SendError(error.Get(),
                                                        /*force=*/true);
                                   },
                                   cppgc::Persistent<IDBRequest>(this),
                                   cppgc::Persistent<DOMException>(error)));
  } else {
    SendError(error, /*force=*/true);
  }
}

void IDBRequest::SetCursorDetails(IDBCursorType cursor_type,
                                  IndexedDbCursorDirection direction) {
  DCHECK_EQ(ready_state_, kPending);
  DCHECK(!pending_cursor_);
  cursor_type_ = cursor_type;
  cursor_direction_ = direction;
}

void IDBRequest::SetPendingCursor(IDBCursor* cursor) {
  DCHECK_EQ(ready_state_, kDone);
  DCHECK(HasContext());
  DCHECK(transaction_);
  DCHECK(!pending_cursor_);
  DCHECK_EQ(cursor, GetResultCursor());

  pending_cursor_ = cursor;
  SetResult(nullptr);
  ready_state_ = kPending;
  error_.Clear();
  transaction_->RegisterRequest(this);
}

IDBCursor* IDBRequest::GetResultCursor() const {
  if (!result_) {
    return nullptr;
  }
  if (result_->GetType() == IDBAny::kIDBCursorType ||
      result_->GetType() == IDBAny::kIDBCursorWithValueType) {
    return result_->Cursor();
  }
  return nullptr;
}

void IDBRequest::SendResultCursorInternal(IDBCursor* cursor,
                                          IDBKey key,
                                          IDBKey primary_key,
                                          IndexedDbReturnValue value) {
  DCHECK_EQ(ready_state_, kPending);
  cursor_key_ = std::move(key);
  cursor_primary_key_ = std::move(primary_key);
  cursor_value_ = std::move(value);

  SendResult(bindings::MakeGarbageCollected<IDBAny>(cursor));
}

bool IDBRequest::CanStillSendResult() const {
  // A result can come after the context went.
  if (!HasContext()) {
    return false;
  }
  DCHECK(ready_state_ == kPending || ready_state_ == kDone);
  if (request_aborted_) {
    return false;
  }
  DCHECK_EQ(ready_state_, kPending);
  DCHECK(!error_ && !result_);
  return true;
}

void IDBRequest::HandleResponse(IDBKey key) {
  transaction_->EnqueueResult(std::make_unique<IDBRequestQueueItem>(
      this, std::move(key), OnResultReadyOf(transaction_.Get())));
}

void IDBRequest::HandleResponse(IndexedDbReturnValue value) {
  transaction_->EnqueueResult(std::make_unique<IDBRequestQueueItem>(
      this, std::move(value), OnResultReadyOf(transaction_.Get())));
}

void IDBRequest::HandleResponse(int64_t value) {
  transaction_->EnqueueResult(std::make_unique<IDBRequestQueueItem>(
      this, value, OnResultReadyOf(transaction_.Get())));
}

void IDBRequest::HandleResponse() {
  transaction_->EnqueueResult(std::make_unique<IDBRequestQueueItem>(
      this, OnResultReadyOf(transaction_.Get())));
}

void IDBRequest::HandleResponseAdvanceCursor(IDBKey key,
                                             IDBKey primary_key,
                                             IndexedDbReturnValue value) {
  transaction_->EnqueueResult(std::make_unique<IDBRequestQueueItem>(
      this, std::move(key), std::move(primary_key), std::move(value),
      OnResultReadyOf(transaction_.Get())));
}

void IDBRequest::HandleError(IndexedDbException code,
                             const std::u16string& message) {
  // When the Session clears a transaction's tasks, a call whose task went
  // hears kIgnorableAbortError, which needs no answer.
  if (code == IndexedDbException::kIgnorableAbortError) {
    return;
  }
  transaction_->EnqueueResult(std::make_unique<IDBRequestQueueItem>(
      this, MakeIDBException(code, message),
      OnResultReadyOf(transaction_.Get())));
}

void IDBRequest::OnClear(const PageIndexedDbCallback& answer) {
  if (answer.success) {
    HandleResponse();
  }
}

void IDBRequest::OnDelete(const PageIndexedDbCallback& answer) {
  if (answer.success) {
    HandleResponse();
  }
}

void IDBRequest::OnCount(const PageIndexedDbCallback& answer) {
  if (answer.success) {
    HandleResponse(static_cast<int64_t>(answer.count));
  }
}

void IDBRequest::OnPut(const PageIndexedDbCallback& answer) {
  if (answer.error != IndexedDbException::kNoError) {
    HandleError(answer.error, answer.message);
    return;
  }
  HandleResponse(answer.key);
}

void IDBRequest::OnGet(const PageIndexedDbCallback& answer) {
  if (answer.error != IndexedDbException::kNoError) {
    HandleError(answer.error, answer.message);
    return;
  }
  if (answer.empty) {
    HandleResponse();
  } else if (answer.value) {
    HandleResponse(*answer.value);
  } else {
    HandleResponse(answer.key);
  }
}

void IDBRequest::OnGetAll(IndexedDbGetAllResultType result_type,
                          const PageIndexedDbCallback& answer) {
  if (!receiving_get_all_) {
    // The call's first chunk, which Blink's OnGetAll() gets with the
    // receiver of the rest, if more come.
    receiving_get_all_ = !answer.done;
    auto item = std::make_unique<IDBRequestQueueItem>(
        this, result_type, OnResultReadyOf(transaction_.Get()));
    const bool failed = answer.error != IndexedDbException::kNoError;
    if (!failed) {
      item->ReceiveResults(answer.records, answer.done);
    }
    transaction_->EnqueueResult(std::move(item));
    // What fails comes through the sink, once the item is queued.
    if (failed && queue_item_ && queue_item_->IsWaitingForRecords()) {
      queue_item_->OnGetAllError(answer.error, answer.message);
    }
    return;
  }
  if (answer.done) {
    receiving_get_all_ = false;
  }
  // An item that was canceled, as its transaction aborted, dropped its sink,
  // and the rest of the records go nowhere.
  if (!queue_item_ || !queue_item_->IsWaitingForRecords()) {
    return;
  }
  if (answer.error != IndexedDbException::kNoError) {
    queue_item_->OnGetAllError(answer.error, answer.message);
    return;
  }
  queue_item_->ReceiveResults(answer.records, answer.done);
}

void IDBRequest::OnOpenCursor(uint64_t cursor,
                              const PageIndexedDbCallback& answer) {
  if (answer.error != IndexedDbException::kNoError) {
    HandleError(answer.error, answer.message);
    return;
  }

  // No record: the result is null.
  if (answer.empty) {
    HandleResponse(IndexedDbReturnValue());
    return;
  }

  // A key cursor's record has no value.
  transaction_->EnqueueResult(std::make_unique<IDBRequestQueueItem>(
      this, cursor, answer.key, answer.primary_key,
      answer.value.value_or(IndexedDbReturnValue()),
      OnResultReadyOf(transaction_.Get())));
}

void IDBRequest::OnAdvanceCursor(const PageIndexedDbCallback& answer) {
  if (answer.error != IndexedDbException::kNoError) {
    HandleError(answer.error, answer.message);
    return;
  }

  if (answer.empty && !answer.keys.empty()) {
    HandleError(IndexedDbException::kUnknownError, u"Invalid response");
    return;
  }

  // One record, whose value only a key cursor's lacks, where Mojo's has a
  // null one.
  if (!answer.empty &&
      (answer.keys.size() != 1u || answer.primary_keys.size() != 1u ||
       answer.values.size() > 1u)) {
    HandleError(IndexedDbException::kUnknownError, u"Invalid response");
    return;
  }

  // The end: the result is null.
  if (answer.empty) {
    HandleResponse(IndexedDbReturnValue());
    return;
  }

  HandleResponseAdvanceCursor(
      answer.keys.front(), answer.primary_keys.front(),
      answer.values.empty() ? IndexedDbReturnValue() : answer.values.front());
}

void IDBRequest::OnGotKeyGeneratorCurrentNumber(
    const PageIndexedDbCallback& answer) {
  if (answer.error != IndexedDbException::kNoError) {
    HandleError(answer.error, answer.message);
    return;
  }
  DCHECK_GE(answer.number, 0);
  HandleResponse(answer.number);
}

void IDBRequest::SendError(DOMException* error, bool force) {
  if (!HasContext() || (request_aborted_ && !force)) {
    return;
  }

  error_ = error;
  SetResult(bindings::MakeGarbageCollected<IDBAny>());
  pending_cursor_.Clear();
  DispatchEvent(MakeIDBEvent(u"error", /*bubbles=*/true, /*cancelable=*/true));
}

void IDBRequest::SendResultCursor(uint64_t cursor_id,
                                  IDBKey key,
                                  IDBKey primary_key,
                                  IndexedDbReturnValue value) {
  if (!CanStillSendResult()) {
    // Blink drops the cursor's pending remote, which closes the Session's.
    if (transaction_) {
      CloseIDBCursor(transaction_->db().Host(), cursor_id);
    }
    return;
  }

  DCHECK(!pending_cursor_);
  // The cursor's source, as Blink's union of the request's holds it.
  IDBObjectStore* source_object_store = nullptr;
  IDBIndex* source_index = nullptr;
  DCHECK(source_);
  const bindings::WrapperTypeInfo* const source_type =
      source_->GetWrapperTypeInfo();
  if (source_type == &bindings::V8IDBIndex::kWrapperTypeInfo) {
    source_index = static_cast<IDBIndex*>(source_.Get());
  } else if (source_type == &bindings::V8IDBObjectStore::kWrapperTypeInfo) {
    source_object_store = static_cast<IDBObjectStore*>(source_.Get());
  }
  CHECK(source_object_store || source_index);

  IDBCursor* cursor = nullptr;
  switch (cursor_type_) {
    case IDBCursorType::kKeyOnly:
      cursor = bindings::MakeGarbageCollected<IDBCursor>(
          cursor_id, cursor_direction_, this, source_object_store, source_index,
          transaction_.Get());
      break;
    case IDBCursorType::kKeyAndValue:
      cursor = bindings::MakeGarbageCollected<IDBCursorWithValue>(
          cursor_id, cursor_direction_, this, source_object_store, source_index,
          transaction_.Get());
      break;
  }
  SendResultCursorInternal(cursor, std::move(key), std::move(primary_key),
                           std::move(value));
}

void IDBRequest::SendResult(IDBAny* result) {
  if (!CanStillSendResult()) {
    return;
  }
  DCHECK(!pending_cursor_);
  SetResult(result);
  DispatchEvent(MakeIDBEvent(u"success", /*bubbles=*/false,
                             /*cancelable=*/false));
}

void IDBRequest::SetResult(IDBAny* result) {
  result_ = result;
  result_dirty_ = true;
  cached_result_.Reset();
}

void IDBRequest::SendResultValue(IndexedDbReturnValue value) {
  if (!CanStillSendResult()) {
    return;
  }
  if (pending_cursor_) {
    // An empty value: the cursor continued past its end.
    DCHECK(value.bits.empty());
    pending_cursor_->Close();
    pending_cursor_.Clear();
  }
  SendResult(bindings::MakeGarbageCollected<IDBAny>(std::move(value)));
}

void IDBRequest::SendResultAdvanceCursor(IDBKey key,
                                         IDBKey primary_key,
                                         IndexedDbReturnValue value) {
  if (!CanStillSendResult()) {
    return;
  }
  DCHECK(pending_cursor_);
  IDBCursor* const cursor = pending_cursor_.Get();
  pending_cursor_.Clear();
  SendResultCursorInternal(cursor, std::move(key), std::move(primary_key),
                           std::move(value));
}

void IDBRequest::ContextDestroyed() {
  if (ready_state_ == kPending) {
    ready_state_ = kEarlyDeath;
    if (queue_item_) {
      queue_item_->CancelLoading();
    }
    if (transaction_) {
      transaction_->UnregisterRequest(this);
    }
  }

  if (IDBCursor* const source_cursor = AsCursor(source_.Get())) {
    source_cursor->ContextWillBeDestroyed();
  }
  if (IDBCursor* const result_cursor = GetResultCursor()) {
    result_cursor->ContextWillBeDestroyed();
  }
  if (pending_cursor_) {
    pending_cursor_->ContextWillBeDestroyed();
  }
}

DispatchEventResult IDBRequest::DispatchEventInternal(Event* event) {
  event->SetTarget(this);

  std::vector<EventTarget*> targets;
  targets.push_back(this);
  if (transaction_ && !prevent_propagation_) {
    // The spec's get the parent: a request's is its transaction, and a
    // transaction's its connection.
    targets.push_back(transaction_.Get());
    targets.push_back(&transaction_->db());
  }

  // An event that a script dispatches has no side effects.
  if (!event->IsTrusted()) {
    return DispatchIDBEvent(*event, targets);
  }
  const std::u16string type = event->Type();
  DCHECK(type == u"success" || type == u"error" || type == u"blocked" ||
         type == u"upgradeneeded");

  if (!HasContext()) {
    return DispatchEventResult::kNotCanceled;
  }
  DCHECK_EQ(ready_state_, kPending);

  if (type != u"blocked") {
    ready_state_ = kDone;
  }

  // A cursor's key, primary key and value change only once its success event
  // is dispatched.
  IDBCursor* cursor_to_notify = nullptr;
  if (type == u"success") {
    cursor_to_notify = GetResultCursor();
    if (cursor_to_notify) {
      cursor_to_notify->SetValueReady(std::move(cursor_key_),
                                      std::move(cursor_primary_key_),
                                      std::move(cursor_value_));
    }
  }

  if (type == u"upgradeneeded") {
    DCHECK(!did_fire_upgrade_needed_event_);
    did_fire_upgrade_needed_event_ = true;
  }

  // Held while listeners run, as they may let go of the request.
  const cppgc::Persistent<IDBTransaction> transaction(transaction_.Get());
  const bool set_transaction_active =
      transaction && (type == u"success" || type == u"upgradeneeded" ||
                      (type == u"error" && !request_aborted_));

  if (set_transaction_active) {
    transaction->SetActive(true);
  }

  // Unregistered before the listeners run, since a cursor's continue() or
  // advance() in one registers the request again.
  if (transaction && ready_state_ == kDone) {
    transaction->UnregisterRequest(this);
  }

  if (type == u"error" && transaction) {
    transaction->IncrementNumErrorsHandled();
  }

  const DispatchEventResult dispatch_result = DispatchIDBEvent(*event, targets);
  // Blink's IndexPopulator, the request's onsuccess, a listener at the
  // request alone that no page can add to.
  if (native_success_listener_ && type == u"success") {
    native_success_listener_->Invoke(this);
  }

  if (transaction) {
    // After unregistering, so that this request hears no second error, and
    // before the transaction goes inactive, which may commit it.
    if (!request_aborted_) {
      // A listener's uncaught exception, or an error that nothing canceled,
      // aborts the transaction.
      if (event->LegacyDidListenersThrow()) {
        transaction->StartAborting(DOMException::Create(
            u"Uncaught exception in event handler.", u"AbortError"));
      } else if (type == u"error" &&
                 dispatch_result == DispatchEventResult::kNotCanceled) {
        transaction->StartAborting(error_.Get());
      }
    }

    // The last request of the transaction may commit it here.
    if (set_transaction_active) {
      transaction->SetActive(false);
    }
  }

  if (cursor_to_notify) {
    cursor_to_notify->PostSuccessHandlerCallback();
  }

  return dispatch_result;
}

void IDBRequest::TransactionDidFinishAndDispatch() {
  DCHECK(transaction_);
  DCHECK(transaction_->IsVersionChange());
  DCHECK(did_fire_upgrade_needed_event_);
  DCHECK_EQ(ready_state_, kDone);
  transaction_.Clear();

  if (!HasContext()) {
    return;
  }

  ready_state_ = kPending;
}

}  // namespace mitto
