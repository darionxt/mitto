#include "mitto/dom/idb_record.h"

#include <utility>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_idb_record.h"
#include "mitto/dom/idb_any.h"
#include "mitto/dom/idb_key_conversion.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

// What a page reads of an attribute, made once and kept.
v8::Local<v8::Value> Cached(v8::Isolate* isolate,
                            v8::TracedReference<v8::Value>& cache,
                            v8::Local<v8::Value> value) {
  if (value.IsEmpty()) {
    value = v8::Undefined(isolate);
  }
  cache.Reset(isolate, value);
  return value;
}

}  // namespace

// static
v8::Local<v8::Value> IDBRecordArray::ToV8(v8::Isolate* isolate,
                                          IDBRecordArray source) {
  // Each record has a primary key and a value, and an index's an index key.
  CHECK_EQ(source.primary_keys.size(), source.values.size());
  CHECK(source.index_keys.size() == source.primary_keys.size() ||
        source.index_keys.empty());

  // On the stack, where the collector finds them.
  std::vector<IDBRecord*> records;
  records.reserve(source.primary_keys.size());
  for (size_t i = 0; i < source.primary_keys.size(); ++i) {
    std::optional<IDBKey> index_key;
    if (!source.index_keys.empty()) {
      index_key = std::move(source.index_keys[i]);
    }
    records.push_back(bindings::MakeGarbageCollected<IDBRecord>(
        std::move(source.primary_keys[i]), std::move(source.values[i]),
        std::move(index_key)));
  }
  return bindings::ToV8Array(isolate, records);
}

IDBRecord::IDBRecord(IDBKey primary_key,
                     IndexedDbReturnValue value,
                     std::optional<IDBKey> index_key)
    : primary_key_(std::move(primary_key)),
      value_(std::move(value)),
      index_key_(std::move(index_key)) {}

IDBRecord::~IDBRecord() = default;

const bindings::WrapperTypeInfo* IDBRecord::GetWrapperTypeInfo() const {
  return &bindings::V8IDBRecord::kWrapperTypeInfo;
}

void IDBRecord::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(cached_key_);
  visitor->Trace(cached_primary_key_);
  visitor->Trace(cached_value_);
}

v8::Local<v8::Value> IDBRecord::Key() {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (!cached_key_.IsEmpty()) {
    return cached_key_.Get(isolate);
  }
  const IDBKey& key = index_key_ ? *index_key_ : primary_key_;
  return Cached(isolate, cached_key_, IDBKeyToV8(isolate, key));
}

v8::Local<v8::Value> IDBRecord::PrimaryKey() {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (!cached_primary_key_.IsEmpty()) {
    return cached_primary_key_.Get(isolate);
  }
  return Cached(isolate, cached_primary_key_,
                IDBKeyToV8(isolate, primary_key_));
}

v8::Local<v8::Value> IDBRecord::Value() {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (!cached_value_.IsEmpty()) {
    return cached_value_.Get(isolate);
  }
  return Cached(isolate, cached_value_, DeserializeIDBValue(isolate, value_));
}

}  // namespace mitto
