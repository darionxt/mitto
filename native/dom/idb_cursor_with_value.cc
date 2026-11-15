#include "mitto/dom/idb_cursor_with_value.h"

#include "mitto/bindings/v8_idb_cursor_with_value.h"

namespace mitto {

IDBCursorWithValue::IDBCursorWithValue(uint64_t cursor,
                                       IndexedDbCursorDirection direction,
                                       IDBRequest* request,
                                       IDBObjectStore* source_object_store,
                                       IDBIndex* source_index,
                                       IDBTransaction* transaction)
    : IDBCursor(cursor,
                direction,
                request,
                source_object_store,
                source_index,
                transaction) {}

IDBCursorWithValue::~IDBCursorWithValue() = default;

const bindings::WrapperTypeInfo* IDBCursorWithValue::GetWrapperTypeInfo()
    const {
  return &bindings::V8IDBCursorWithValue::kWrapperTypeInfo;
}

bool IDBCursorWithValue::IsKeyCursor() const {
  return false;
}

bool IDBCursorWithValue::IsCursorWithValue() const {
  return true;
}

}  // namespace mitto
