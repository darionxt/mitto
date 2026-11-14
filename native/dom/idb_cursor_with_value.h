// IndexedDB's IDBCursorWithValue, as Blink's: the cursor that openCursor()
// makes, which has its record's value too. Its value attribute lives in
// IDBCursor, as Blink keeps it, so that the cursor's code stays in one place.
//
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_cursor_with_value.h
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_cursor_with_value.cc

#ifndef MITTO_DOM_IDB_CURSOR_WITH_VALUE_H_
#define MITTO_DOM_IDB_CURSOR_WITH_VALUE_H_

#include <stdint.h>

#include "mitto/dom/idb_cursor.h"
#include "mitto/indexed_db_messages.h"

namespace mitto {

class IDBIndex;
class IDBObjectStore;
class IDBRequest;
class IDBTransaction;

class IDBCursorWithValue final : public IDBCursor {
 public:
  IDBCursorWithValue(uint64_t cursor,
                     IndexedDbCursorDirection direction,
                     IDBRequest* request,
                     IDBObjectStore* source_object_store,
                     IDBIndex* source_index,
                     IDBTransaction* transaction);
  ~IDBCursorWithValue() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  bool IsKeyCursor() const override;
  bool IsCursorWithValue() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_IDB_CURSOR_WITH_VALUE_H_
