// The result of an IndexedDB request, as Blink's IDBAny holds it until a
// script reads IDBRequest.result: nothing, a connection, a cursor, a key, a
// value as the Session stored it, a list of values or of records, or a number.
// A value is deserialized only when the result is read, in the reader's
// context, with the primary key that a key generator made put into it.
//
// Chrome wraps a value over 64 KiB in a Blob, which IDBRequestLoader unwraps
// before the result is ready; the Session sends values whole, so nothing here
// waits for that.
//
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_any.h
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_any.cc
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_record_array.h
// Upstream: third_party/blink/renderer/bindings/modules/v8/v8_binding_for_modules.cc

#ifndef MITTO_DOM_IDB_ANY_H_
#define MITTO_DOM_IDB_ANY_H_

#include <stdint.h>

#include <vector>

#include "mitto/indexed_db_key.h"
#include "mitto/indexed_db_messages.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-local-handle.h"

namespace mitto {

class IDBCursor;
class IDBDatabase;

// Blink's IDBRecordArray: getAllRecords()'s results, the primary keys, the
// values and, for an index, the index keys, in parallel.
struct IDBRecordArray {
  IDBRecordArray();
  IDBRecordArray(IDBRecordArray&&);
  IDBRecordArray& operator=(IDBRecordArray&&);
  ~IDBRecordArray();

  // A new array of an IDBRecord of each record, which takes the record's
  // keys and value from `source`, in the current context. idb_record.cc has
  // it, with IDBRecord.
  static v8::Local<v8::Value> ToV8(v8::Isolate* isolate, IDBRecordArray source);

  void clear();

  std::vector<IDBKey> primary_keys;
  std::vector<IndexedDbReturnValue> values;
  std::vector<IDBKey> index_keys;
};

// DeserializeIDBValue(): a copy of the stored `value` in the current context,
// with the primary key that a key generator made put into it at its key path.
// Null if it cannot be read back, as for an empty value, which Blink gives
// for no value.
v8::Local<v8::Value> DeserializeIDBValue(v8::Isolate* isolate,
                                         const IndexedDbReturnValue& value);

// DeserializeIDBValueArray(): a new array of the values, deserialized.
v8::Local<v8::Value> DeserializeIDBValueArray(
    v8::Isolate* isolate,
    const std::vector<IndexedDbReturnValue>& values);

class IDBAny final : public cppgc::GarbageCollected<IDBAny> {
 public:
  enum Type {
    kUndefinedType = 0,
    kIDBCursorType,
    kIDBCursorWithValueType,
    kIDBDatabaseType,
    kIntegerType,
    kKeyType,
    kIDBValueType,
    kIDBValueArrayType,
    kIDBRecordArrayType,
  };

  // Undefined.
  IDBAny();
  explicit IDBAny(IDBDatabase* database);
  // An IDBCursor, or an IDBCursorWithValue.
  explicit IDBAny(IDBCursor* cursor);
  explicit IDBAny(IDBKey key);
  explicit IDBAny(IndexedDbReturnValue value);
  explicit IDBAny(std::vector<IndexedDbReturnValue> values);
  explicit IDBAny(int64_t integer);
  explicit IDBAny(IDBRecordArray records);
  ~IDBAny();

  void Trace(cppgc::Visitor* visitor) const;

  Type GetType() const { return type_; }
  IDBCursor* Cursor() const { return cursor_.Get(); }
  IDBDatabase* IdbDatabase() const { return database_.Get(); }
  const IDBKey& Key() const { return key_; }
  const IndexedDbReturnValue& Value() const { return values_.front(); }
  const std::vector<IndexedDbReturnValue>& Values() const { return values_; }
  const IDBRecordArray& Records() const { return records_; }
  int64_t Integer() const { return integer_; }

  // The result as a page sees it, in the current context: undefined, the
  // object's wrapper, the key's value, the value deserialized, an array of
  // values, or a Number.
  v8::Local<v8::Value> ToV8(v8::Isolate* isolate);

 private:
  Type type_ = kUndefinedType;
  cppgc::Member<IDBCursor> cursor_;
  cppgc::Member<IDBDatabase> database_;
  IDBKey key_;
  // One for a value.
  std::vector<IndexedDbReturnValue> values_;
  IDBRecordArray records_;
  int64_t integer_ = 0;
};

}  // namespace mitto

#endif  // MITTO_DOM_IDB_ANY_H_
