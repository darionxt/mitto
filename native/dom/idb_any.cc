#include "mitto/dom/idb_any.h"

#include <stdint.h>

#include <tuple>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/notreached.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/dom/idb_cursor.h"
#include "mitto/dom/idb_database.h"
#include "mitto/dom/idb_key_conversion.h"
#include "mitto/dom/serialized_script_value.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

IDBRecordArray::IDBRecordArray() = default;
IDBRecordArray::IDBRecordArray(IDBRecordArray&&) = default;
IDBRecordArray& IDBRecordArray::operator=(IDBRecordArray&&) = default;
IDBRecordArray::~IDBRecordArray() = default;

void IDBRecordArray::clear() {
  primary_keys.clear();
  values.clear();
  index_keys.clear();
}

v8::Local<v8::Value> DeserializeIDBValue(v8::Isolate* isolate,
                                         const IndexedDbReturnValue& value) {
  // Values over 64 KiB come whole from the Session, where Chrome would have
  // wrapped them in a Blob that IDBRequestLoader unwraps before this.
  const SerializedScriptValue serialized = SerializedScriptValue::FromWireData(
      std::vector<uint8_t>(value.bits.begin(), value.bits.end()));
  const v8::Local<v8::Value> v8_value = serialized.Deserialize(isolate);
  if (value.primary_key.type() != IDBKeyType::kNone &&
      !value.key_path.IsNull()) {
    // The primary key has not been put into the value yet, so put it in. A
    // stored value that does not suit its store takes none, as Blink's
    // DCHECK lets go in a release build.
    const v8::Local<v8::Value> key = IDBKeyToV8(isolate, value.primary_key);
    if (!key.IsEmpty()) {
      std::ignore =
          InjectV8KeyIntoV8Value(isolate, key, v8_value, value.key_path);
    }
  }
  return v8_value;
}

v8::Local<v8::Value> DeserializeIDBValueArray(
    v8::Isolate* isolate,
    const std::vector<IndexedDbReturnValue>& values) {
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::Local<v8::Array> array =
      v8::Array::New(isolate, static_cast<int>(values.size()));
  for (size_t i = 0; i < values.size(); ++i) {
    v8::Local<v8::Value> v8_value = DeserializeIDBValue(isolate, values[i]);
    if (v8_value.IsEmpty()) {
      v8_value = v8::Undefined(isolate);
    }
    bool created_property = false;
    if (!array->CreateDataProperty(context, static_cast<uint32_t>(i), v8_value)
             .To(&created_property) ||
        !created_property) {
      return {};
    }
  }
  return array;
}

IDBAny::IDBAny() = default;

IDBAny::IDBAny(IDBDatabase* database)
    : type_(kIDBDatabaseType), database_(database) {}

IDBAny::IDBAny(IDBCursor* cursor)
    : type_(cursor->IsCursorWithValue() ? kIDBCursorWithValueType
                                        : kIDBCursorType),
      cursor_(cursor) {}

IDBAny::IDBAny(IDBKey key) : type_(kKeyType), key_(std::move(key)) {}

IDBAny::IDBAny(IndexedDbReturnValue value) : type_(kIDBValueType) {
  values_.push_back(std::move(value));
}

IDBAny::IDBAny(std::vector<IndexedDbReturnValue> values)
    : type_(kIDBValueArrayType), values_(std::move(values)) {}

IDBAny::IDBAny(int64_t integer) : type_(kIntegerType), integer_(integer) {}

IDBAny::IDBAny(IDBRecordArray records)
    : type_(kIDBRecordArrayType), records_(std::move(records)) {}

IDBAny::~IDBAny() = default;

void IDBAny::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(cursor_);
  visitor->Trace(database_);
}

v8::Local<v8::Value> IDBAny::ToV8(v8::Isolate* isolate) {
  switch (type_) {
    case kUndefinedType:
      return v8::Undefined(isolate);
    case kIDBCursorType:
    case kIDBCursorWithValueType:
      return bindings::ToV8(isolate, cursor_.Get());
    case kIDBDatabaseType:
      return bindings::ToV8(isolate, database_.Get());
    case kKeyType:
      return IDBKeyToV8(isolate, key_);
    case kIDBValueType:
      return DeserializeIDBValue(isolate, values_.front());
    case kIDBValueArrayType:
      return DeserializeIDBValueArray(isolate, values_);
    case kIntegerType:
      return v8::Number::New(isolate, static_cast<double>(integer_));
    case kIDBRecordArrayType:
      // Takes the records, as Blink's does, which IDBRequest's cache of its
      // result reads once.
      return IDBRecordArray::ToV8(isolate, std::move(records_));
  }
  NOTREACHED();
}

}  // namespace mitto
