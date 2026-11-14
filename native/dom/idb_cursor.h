// IndexedDB's IDBCursor, as Blink's: a cursor over an object store or an
// index, in a direction, whose key, primary key and, for an
// IDBCursorWithValue, value are those of the record it is at, from the
// success event that brought it there. advance(), continue() and
// continuePrimaryKey() reuse the request that opened it; update() and
// delete() make new ones. Once a cursor has had more than two continue()
// calls without a key, it asks the Session for 5 records at once, then twice
// as many each time up to 100, and serves continue() from them until anything
// else happens on its transaction, which hands back those it did not use.
//
// The Page names the Session's cursor by the number it gave the open that
// made it; dropping Blink's remote of it closes it, as kCloseCursor does
// here, when the cursor ends, its request gives up its result, or the
// collector takes it.
//
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_cursor.h
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_cursor.cc

#ifndef MITTO_DOM_IDB_CURSOR_H_
#define MITTO_DOM_IDB_CURSOR_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/indexed_db_key.h"
#include "mitto/indexed_db_messages.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/prefinalizer.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-traced-handle.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class IDBIndex;
class IDBObjectStore;
class IDBRequest;
class IDBTransaction;
class IndexedDbHost;

// Closes the Session's cursor numbered `cursor`, as dropping Blink's remote
// of an IDBCursor does.
void CloseIDBCursor(IndexedDbHost* host, uint64_t cursor);

class IDBCursor : public bindings::ScriptWrappable {
  CPPGC_USING_PRE_FINALIZER(IDBCursor, Dispose);

 public:
  // V8EnumToDirection(): an IDBCursorDirection, which the bindings checked.
  static IndexedDbCursorDirection DirectionFromString(
      const std::u16string& direction);

  // The cursor numbered `cursor` that `request` opened, over
  // `source_object_store` or `source_index`, one of which is null.
  IDBCursor(uint64_t cursor,
            IndexedDbCursorDirection direction,
            IDBRequest* request,
            IDBObjectStore* source_object_store,
            IDBIndex* source_index,
            IDBTransaction* transaction);
  ~IDBCursor() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;
  void ContextWillBeDestroyed();

  // The members of the interface.
  // The IDBObjectStore or the IDBIndex.
  bindings::ScriptWrappable* Source() const;
  std::u16string Direction() const;
  v8::Local<v8::Value> Key();
  v8::Local<v8::Value> PrimaryKey();
  IDBRequest* Request();
  void Advance(uint32_t count, bindings::ExceptionState& exception_state);
  void Continue(v8::Local<v8::Value> key,
                bindings::ExceptionState& exception_state);
  void ContinuePrimaryKey(v8::Local<v8::Value> key,
                          v8::Local<v8::Value> primary_key,
                          bindings::ExceptionState& exception_state);
  IDBRequest* Update(v8::Local<v8::Value> value,
                     bindings::ExceptionState& exception_state);
  IDBRequest* Delete(bindings::ExceptionState& exception_state);
  // IDBCursorWithValue's value, which Blink keeps here too.
  v8::Local<v8::Value> Value();

  // What the rest of IndexedDB uses.
  //
  // Moves on by one record, or past `key`, and with it `primary_key`, each
  // of which may be None, once the checks of continue() have passed.
  void Continue(IDBKey key,
                IDBKey primary_key,
                bindings::ExceptionState& exception_state);
  // After a success event of the cursor's request has been dispatched.
  void PostSuccessHandlerCallback();
  // Whether its source, or its index's object store, has been deleted.
  bool IsDeleted() const;
  // It went past its end.
  void Close();
  // The record that the cursor is at, from the success event that brings it
  // there.
  void SetValueReady(IDBKey key,
                     IDBKey primary_key,
                     IndexedDbReturnValue value);
  const IDBKey& IdbPrimaryKey() const { return primary_key_; }
  virtual bool IsKeyCursor() const;
  virtual bool IsCursorWithValue() const;
  // Drops the records that a prefetch brought, and has the Session's cursor
  // go back to the one after the last that was used.
  void ResetPrefetchCache();

 private:
  // Which method gets an answer of the Session's cursor, with the request
  // that the call was made for.
  enum class AnswerTo { kAdvanceCallback, kPrefetchCallback };

  // advance(), continue() and continuePrimaryKey(), through the prefetched
  // records or the Session.
  void AdvanceImpl(uint32_t count, IDBRequest* request);
  void CursorContinue(const IDBKey& key,
                      const IDBKey& primary_key,
                      IDBRequest* request);
  void PrefetchCallback(IDBRequest* request, PageIndexedDbCallback answer);
  void SetPrefetchData(std::vector<IDBKey> keys,
                       std::vector<IDBKey> primary_keys,
                       std::vector<IndexedDbReturnValue> values);
  void CachedAdvance(uint32_t count, IDBRequest* request);
  void CachedContinue(IDBRequest* request);
  void AdvanceCallback(IDBRequest* request, PageIndexedDbCallback answer);

  IDBObjectStore* EffectiveObjectStore() const;
  // Throws, as Blink's do, unless the cursor can move or, with
  // `read_only_error_message`, write.
  bool CheckForCommonExceptions(bindings::ExceptionState& exception_state,
                                const char* read_only_error_message);

  // Sends `call` of the Session's cursor, whose answer goes to `answer_to`
  // in the window's context, and what nothing answers. Blink's cursor is
  // associated with its connection's interface, so nothing goes once the
  // connection has closed.
  void Ask(PageIndexedDbCall call, IDBRequest* request, AnswerTo answer_to);
  void Tell(PageIndexedDbCall call);
  void OnAnswer(IDBRequest* request,
                AnswerTo answer_to,
                PageIndexedDbCallback answer);
  // A cursor that the collector takes closes, as dropping Blink's remote
  // does.
  void Dispose();

  static constexpr int kPrefetchContinueThreshold = 2;
  static constexpr int kMinPrefetchAmount = 5;
  static constexpr int kMaxPrefetchAmount = 100;

  // The records a prefetch brought, last first, so that a continue() takes
  // the one at the back.
  std::vector<IDBKey> prefetch_keys_;
  std::vector<IDBKey> prefetch_primary_keys_;
  std::vector<IndexedDbReturnValue> prefetch_values_;
  // The continue() calls in a row that could be prefetched.
  int continue_count_ = 0;
  // How many records of the last prefetch were used.
  int used_prefetches_ = 0;
  // The success events that are due before the prefetched records are
  // dropped.
  int pending_onsuccess_callbacks_ = 0;
  // How many records the next prefetch asks for.
  int prefetch_amount_ = kMinPrefetchAmount;

  const raw_ptr<IndexedDbHost> host_;
  // The Session's cursor, while Blink's remote would be bound.
  const uint64_t cursor_;
  bool remote_bound_ = true;
  cppgc::Member<IDBRequest> request_;
  // [SaveSameObject]: what request gave first, which it gives from then on,
  // null for a cursor that had closed, as Blink's bindings keep it.
  cppgc::Member<IDBRequest> saved_request_;
  bool request_saved_ = false;
  const IndexedDbCursorDirection direction_;
  const cppgc::Member<IDBObjectStore> source_object_store_;
  const cppgc::Member<IDBIndex> source_index_;
  const cppgc::Member<IDBTransaction> transaction_;
  bool got_value_ = false;
  // [CachedAttribute]: what key, primaryKey and value gave, which they give
  // again until the cursor moves.
  bool key_dirty_ = true;
  bool primary_key_dirty_ = true;
  bool value_dirty_ = true;
  v8::TracedReference<v8::Value> cached_key_;
  v8::TracedReference<v8::Value> cached_primary_key_;
  v8::TracedReference<v8::Value> cached_value_;
  IDBKey key_;
  IDBKey primary_key_;
  // An IDBCursorWithValue's, with the primary key to put into it at the
  // store's key path when its key generator made the key, as Blink's
  // IDBValue holds it.
  std::optional<IndexedDbReturnValue> value_;
};

}  // namespace mitto

#endif  // MITTO_DOM_IDB_CURSOR_H_
