// A result of an IndexedDB request that waits in its transaction's queue, so
// that the requests' events fire in the order the requests were made, as
// Blink's IDBRequestQueueItem holds one. In Chrome a result can wait for its
// value to be unwrapped from a Blob; the Session sends values whole, so here
// only a getAll() that has not had its last chunk yet keeps the results
// behind it waiting.
//
// The item holds its request, and the transaction holds the item, so the
// transaction's queue has to be drained, which it is once each result is
// ready or canceled.
//
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_request_queue_item.h
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_request_queue_item.cc

#ifndef MITTO_DOM_IDB_REQUEST_QUEUE_ITEM_H_
#define MITTO_DOM_IDB_REQUEST_QUEUE_ITEM_H_

#include <stdint.h>

#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "mitto/dom/idb_any.h"
#include "mitto/indexed_db_key.h"
#include "mitto/indexed_db_messages.h"
#include "v8/include/cppgc/persistent.h"

namespace mitto {

class DOMException;
class IDBRequest;

class IDBRequestQueueItem {
 public:
  IDBRequestQueueItem(IDBRequest* request,
                      DOMException* error,
                      base::OnceClosure on_result_ready);
  IDBRequestQueueItem(IDBRequest* request,
                      int64_t value,
                      base::OnceClosure on_result_ready);
  // Undefined.
  IDBRequestQueueItem(IDBRequest* request, base::OnceClosure on_result_ready);
  IDBRequestQueueItem(IDBRequest* request,
                      IDBKey key,
                      base::OnceClosure on_result_ready);
  IDBRequestQueueItem(IDBRequest* request,
                      IndexedDbReturnValue value,
                      base::OnceClosure on_result_ready);
  // A cursor's next record, after advance(), continue() or
  // continuePrimaryKey().
  IDBRequestQueueItem(IDBRequest* request,
                      IDBKey key,
                      IDBKey primary_key,
                      IndexedDbReturnValue value,
                      base::OnceClosure on_result_ready);
  // An openCursor()'s or openKeyCursor()'s first record, of the Session's
  // cursor numbered `cursor`, which closes if the result goes unsent.
  IDBRequestQueueItem(IDBRequest* request,
                      uint64_t cursor,
                      IDBKey key,
                      IDBKey primary_key,
                      IndexedDbReturnValue value,
                      base::OnceClosure on_result_ready);
  // A getAll(), getAllKeys() or getAllRecords(), whose records come in
  // chunks through ReceiveResults() until the last.
  IDBRequestQueueItem(IDBRequest* request,
                      IndexedDbGetAllResultType get_all_result_type,
                      base::OnceClosure on_result_ready);

  IDBRequestQueueItem(const IDBRequestQueueItem&) = delete;
  IDBRequestQueueItem& operator=(const IDBRequestQueueItem&) = delete;
  ~IDBRequestQueueItem();

  // False while the result still waits.
  bool IsReady() const { return ready_; }
  IDBRequest* Request() const { return request_.Get(); }

  // Starts, once the item is in its transaction's queue, since the result can
  // be ready at once, which tells the transaction.
  void StartLoading();
  // Gives the result up, which makes the item ready with nothing to send.
  void CancelLoading();
  // Calls the request's SendResult() or SendError() with the result. Only
  // the request's transaction calls this.
  void SendResult();

  // IDBDatabaseGetAllResultSinkImpl's: whether a getAll() still waits for
  // records, a chunk of them, and an error in place of the rest.
  bool IsWaitingForRecords() const { return get_all_active_; }
  void ReceiveResults(const std::vector<IndexedDbRecord>& results, bool done);
  void OnGetAllError(IndexedDbException code, const std::u16string& message);

 private:
  // Which of the request's methods gets the result.
  enum ResponseType {
    kCanceled,
    kCursorKeyPrimaryKeyValue,
    kError,
    kKeyPrimaryKeyValue,
    kNumber,
    kKey,
    kKeyArray,
    kRecordArray,
    kValue,
    kValueArray,
    kVoid,
  };

  void OnResultReady();

  cppgc::Persistent<IDBRequest> request_;
  // kError's.
  cppgc::Persistent<DOMException> error_;
  // kKey's, kKeyPrimaryKeyValue's and kCursorKeyPrimaryKeyValue's.
  IDBKey key_;
  // kKeyPrimaryKeyValue's and kCursorKeyPrimaryKeyValue's.
  IDBKey primary_key_;
  // A value for kValue, kKeyPrimaryKeyValue and kCursorKeyPrimaryKeyValue,
  // the primary keys for kKeyArray, the values for kValueArray, and all three
  // for kRecordArray.
  IDBRecordArray records_;
  base::OnceClosure on_result_ready_;
  // kCursorKeyPrimaryKeyValue's Session cursor, as Blink's item holds its
  // pending remote, until the result goes; 0 for none.
  uint64_t pending_cursor_ = 0;
  int64_t int64_value_ = 0;
  ResponseType response_type_ = kVoid;
  bool ready_ = false;
  bool started_loading_ = false;
  // A getAll()'s: whether more records come, which kind it asked for,
  // whether a chunk came, as the first comes before the item is queued, and
  // the records so far.
  bool get_all_active_ = false;
  IndexedDbGetAllResultType get_all_result_type_ =
      IndexedDbGetAllResultType::kValues;
  bool get_all_received_chunk_ = false;
  IDBRecordArray get_all_records_;
};

}  // namespace mitto

#endif  // MITTO_DOM_IDB_REQUEST_QUEUE_ITEM_H_
