// IndexedDB's IDBRequest, as Blink's: a request of a transaction, made by an
// object store, an index or a cursor, which fires success or error once the
// Session has answered, in the order the transaction's requests were made,
// at the request, then its transaction, then its connection. While its
// events are dispatched, its transaction is active; an exception that a
// listener leaves uncaught, or an error event that nothing cancels, aborts
// the transaction.
//
// The object store's, the index's and the cursor's parts of IndexedDB make
// requests through Create(), with themselves as the source, and hand what the
// Session answers to the On*() methods, or to HandleResponse(). The request
// of an openCursor() makes its IDBCursor or IDBCursorWithValue with the first
// record, and the cursor's advance(), continue() and continuePrimaryKey()
// reuse it, setting the cursor's record before each success event.
//
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_request.h
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_request.cc

#ifndef MITTO_DOM_IDB_REQUEST_H_
#define MITTO_DOM_IDB_REQUEST_H_

#include <stdint.h>

#include <string>

#include "base/memory/raw_ptr.h"
#include "mitto/dom/event_target.h"
#include "mitto/indexed_db_key.h"
#include "mitto/indexed_db_messages.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-traced-handle.h"
#include "v8/include/v8-value.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class DOMException;
class IDBAny;
class IDBCursor;
class IDBRequestQueueItem;
class IDBTransaction;
class Window;

// A DOMException of an IndexedDB error that the Session reports, with
// `message`: a QuotaExceededError for kQuotaError, as Chrome makes one with
// QuotaExceededErrorUpdate, and otherwise one whose name is the error's.
DOMException* MakeIDBException(IndexedDbException code,
                               const std::u16string& message);

// Blink's indexed_db::CursorType: the cursor that an openCursor() makes, with
// values, or an openKeyCursor(), without.
enum class IDBCursorType : uint8_t { kKeyAndValue, kKeyOnly };

class IDBRequest : public EventTarget {
 public:
  // The IDL's readyState, and kEarlyDeath for a request whose context went
  // while it was pending.
  enum ReadyStateValue { kPending = 1, kDone = 2, kEarlyDeath = 3 };

  // A listener of the request's success events that is not a page's, as
  // createIndex() sets Blink's IndexPopulator, a NativeEventListener, as the
  // onsuccess of the request whose cursor fills a new index. No page reaches
  // that request, so it runs after the dispatch, while the transaction is
  // still active.
  class NativeSuccessListener
      : public cppgc::GarbageCollected<NativeSuccessListener> {
   public:
    virtual ~NativeSuccessListener() = default;
    virtual void Trace(cppgc::Visitor* visitor) const {}
    virtual void Invoke(IDBRequest* request) = 0;
  };

  // A request of `transaction` in its window, made by `source`, an
  // IDBObjectStore, an IDBIndex or an IDBCursor, which joins the
  // transaction's requests. `transaction` is active.
  static IDBRequest* Create(bindings::ScriptWrappable* source,
                            IDBTransaction* transaction);

  IDBRequest(Window* window,
             bindings::ScriptWrappable* source,
             IDBTransaction* transaction);
  ~IDBRequest() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  v8::Local<v8::Value> Result(bindings::ExceptionState& exception_state);
  DOMException* Error(bindings::ExceptionState& exception_state) const;
  // An IDBObjectStore, an IDBIndex, an IDBCursor, or null.
  bindings::ScriptWrappable* Source() const;
  IDBTransaction* Transaction() const { return transaction_.Get(); }
  std::u16string ReadyState() const;

  // What the rest of IndexedDB uses.
  Window* GetWindow() const { return window_.Get(); }
  // ExecutionContextLifecycleObserver::GetExecutionContext(): whether the
  // request's window still has its context.
  bool HasContext() const;
  ReadyStateValue GetReadyState() const { return ready_state_; }
  bool IsAborted() const { return request_aborted_; }
  bool IsResultDirty() const { return result_dirty_; }
  IDBAny* ResultAsAny() const { return result_.Get(); }
  // Requests made while an index is populated are not the page's: their
  // events reach no transaction or connection.
  void PreventPropagation() { prevent_propagation_ = true; }
  void SetNativeSuccessListener(NativeSuccessListener* listener);

  // Before the result of an openCursor() or an openKeyCursor(): the kind of
  // cursor it makes, and its direction.
  void SetCursorDetails(IDBCursorType cursor_type,
                        IndexedDbCursorDirection direction);
  // A cursor's advance(), continue() and continuePrimaryKey() reuse the
  // request that opened it, which goes back to pending and joins its
  // transaction's requests again.
  void SetPendingCursor(IDBCursor* cursor);

  // Step 5 of the spec's "abort a transaction": an error event of AbortError,
  // from a task of its own when `queue_dispatch`, unless the request is done.
  void Abort(bool queue_dispatch);

  // What the Session answered, which goes to the transaction's queue of
  // results, so that the events fire in the order of the requests.
  void HandleResponse(IDBKey key);
  void HandleResponse(IndexedDbReturnValue value);
  void HandleResponse(int64_t value);
  void HandleResponse();
  // An error of `code`, kUnknownError with "Invalid response" for an answer
  // that does not hold together, and nothing for kIgnorableAbortError.
  void HandleError(IndexedDbException code, const std::u16string& message);

  // The answers to IDBDatabase's and IDBTransaction's calls for the request.
  void OnClear(const PageIndexedDbCallback& answer);
  void OnDelete(const PageIndexedDbCallback& answer);
  void OnCount(const PageIndexedDbCallback& answer);
  void OnPut(const PageIndexedDbCallback& answer);
  void OnGet(const PageIndexedDbCallback& answer);
  // Each chunk of a getAll(), getAllKeys() or getAllRecords(), as
  // `result_type` asked, until the one that is done.
  void OnGetAll(IndexedDbGetAllResultType result_type,
                const PageIndexedDbCallback& answer);
  void OnGotKeyGeneratorCurrentNumber(const PageIndexedDbCallback& answer);
  // An openCursor()'s or openKeyCursor()'s first record, of the Session's
  // cursor numbered `cursor`, and a cursor's next record, or its end.
  void OnOpenCursor(uint64_t cursor, const PageIndexedDbCallback& answer);
  void OnAdvanceCursor(const PageIndexedDbCallback& answer);
  // A cursor's next record, which a prefetch brought, from a task of its
  // own.
  void HandleResponseAdvanceCursor(IDBKey key,
                                   IDBKey primary_key,
                                   IndexedDbReturnValue value);

  // Sets the result and fires success, or sets the error and fires error,
  // as the transaction's queue does once a result is ready. `force` fires
  // even for a request that its transaction aborted.
  void SendResult(IDBAny* result);
  void SendError(DOMException* error, bool force = false);

  // A version change transaction that has finished, and its events have
  // fired, sets its open request back from done to pending, for the success
  // or error that follows.
  void TransactionDidFinishAndDispatch();

  // The window's context went: a pending request gives up its result, which
  // lets its transaction's queue drain.
  void ContextDestroyed();

 protected:
  virtual bool CanStillSendResult() const;
  void SetResult(IDBAny* result);

  // EventTarget:
  DispatchEventResult DispatchEventInternal(Event* event) override;

  // Null for a request of no transaction: a delete, and an open that is done
  // or failed.
  cppgc::Member<IDBTransaction> transaction_;
  ReadyStateValue ready_state_ = kPending;
  // A request that its transaction aborted may still hear its answer, which
  // it then ignores.
  bool request_aborted_ = false;

 private:
  friend class IDBRequestQueueItem;

  // A value, after a cursor's end too.
  void SendResultValue(IndexedDbReturnValue value);
  // The cursor that the result holds, or null.
  IDBCursor* GetResultCursor() const;
  // An openCursor()'s result: a new cursor at its first record.
  void SendResultCursor(uint64_t cursor,
                        IDBKey key,
                        IDBKey primary_key,
                        IndexedDbReturnValue value);
  // The pending cursor, at its next record.
  void SendResultAdvanceCursor(IDBKey key,
                               IDBKey primary_key,
                               IndexedDbReturnValue value);
  void SendResultCursorInternal(IDBCursor* cursor,
                                IDBKey key,
                                IDBKey primary_key,
                                IndexedDbReturnValue value);

  const cppgc::WeakMember<Window> window_;
  const cppgc::Member<bindings::ScriptWrappable> source_;
  cppgc::Member<IDBAny> result_;
  cppgc::Member<DOMException> error_;
  // What result gave a script, which it gives again until the result changes,
  // as Blink's [CachedAttribute=isResultDirty] keeps it.
  v8::TracedReference<v8::Value> cached_result_;
  bool result_dirty_ = true;
  bool did_fire_upgrade_needed_event_ = false;
  bool prevent_propagation_ = false;
  // Whether a getAll()'s first chunk has come and its last not yet.
  bool receiving_get_all_ = false;
  cppgc::Member<NativeSuccessListener> native_success_listener_;
  // An openCursor()'s.
  IDBCursorType cursor_type_ = IDBCursorType::kKeyAndValue;
  IndexedDbCursorDirection cursor_direction_ = IndexedDbCursorDirection::kNext;
  // The cursor whose advance() or continue() made the request pending again.
  cppgc::Member<IDBCursor> pending_cursor_;
  // The record that the cursor of the result is at, which it takes when the
  // success event is dispatched.
  IDBKey cursor_key_;
  IDBKey cursor_primary_key_;
  IndexedDbReturnValue cursor_value_;
  // Not null while the request's result waits in its transaction's queue,
  // which owns the item.
  raw_ptr<IDBRequestQueueItem> queue_item_ = nullptr;
};

}  // namespace mitto

#endif  // MITTO_DOM_IDB_REQUEST_H_
