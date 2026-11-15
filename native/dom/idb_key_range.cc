#include "mitto/dom/idb_key_range.h"

#include <utility>

#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_idb_key_range.h"
#include "mitto/dom/idb_key_conversion.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-value.h"

namespace mitto {

namespace {

// A bound as a page reads it: Blink gives an empty ScriptValue for an absent
// one, which a page reads as undefined.
v8::Local<v8::Value> BoundValue(const IDBKey& bound) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (bound.type() == IDBKeyType::kNone) {
    return v8::Undefined(isolate);
  }
  return IDBKeyToV8(isolate, bound);
}

IDBKeyRange* MakeRange(IDBKey lower,
                       IDBKey upper,
                       bool lower_open,
                       bool upper_open) {
  return bindings::MakeGarbageCollected<IDBKeyRange>(IndexedDbKeyRange(
      std::move(lower), std::move(upper), lower_open, upper_open));
}

}  // namespace

IDBKeyRange::IDBKeyRange(IndexedDbKeyRange range) : range_(std::move(range)) {}

IDBKeyRange::~IDBKeyRange() = default;

// static
bool IDBKeyRange::FromScriptValue(v8::Isolate* isolate,
                                  v8::Local<v8::Value> value,
                                  bindings::ExceptionState& exception_state,
                                  std::optional<IndexedDbKeyRange>* range) {
  if (value->IsNullOrUndefined()) {
    range->reset();
    return true;
  }
  if (IDBKeyRange* const key_range =
          bindings::V8IDBKeyRange::ToWrappable(isolate, value)) {
    *range = key_range->range_;
    return true;
  }
  IDBKey key;
  if (!CreateValidIDBKeyFromValue(isolate, value, exception_state, &key)) {
    return false;
  }
  *range = IndexedDbKeyRange::Only(std::move(key));
  return true;
}

const bindings::WrapperTypeInfo* IDBKeyRange::GetWrapperTypeInfo() const {
  return &bindings::V8IDBKeyRange::kWrapperTypeInfo;
}

v8::Local<v8::Value> IDBKeyRange::LowerValue() const {
  return BoundValue(range_.lower);
}

v8::Local<v8::Value> IDBKeyRange::UpperValue() const {
  return BoundValue(range_.upper);
}

// static
IDBKeyRange* IDBKeyRange::Only(v8::Local<v8::Value> value,
                               bindings::ExceptionState& exception_state) {
  IDBKey key;
  if (!CreateValidIDBKeyFromValue(v8::Isolate::GetCurrent(), value,
                                  exception_state, &key)) {
    return nullptr;
  }
  return bindings::MakeGarbageCollected<IDBKeyRange>(
      IndexedDbKeyRange::Only(std::move(key)));
}

// static
IDBKeyRange* IDBKeyRange::LowerBound(
    v8::Local<v8::Value> bound,
    bool open,
    bindings::ExceptionState& exception_state) {
  IDBKey key;
  if (!CreateValidIDBKeyFromValue(v8::Isolate::GetCurrent(), bound,
                                  exception_state, &key)) {
    return nullptr;
  }
  return MakeRange(std::move(key), IDBKey(), open, /*upper_open=*/true);
}

// static
IDBKeyRange* IDBKeyRange::UpperBound(
    v8::Local<v8::Value> bound,
    bool open,
    bindings::ExceptionState& exception_state) {
  IDBKey key;
  if (!CreateValidIDBKeyFromValue(v8::Isolate::GetCurrent(), bound,
                                  exception_state, &key)) {
    return nullptr;
  }
  return MakeRange(IDBKey(), std::move(key), /*lower_open=*/true, open);
}

// static
IDBKeyRange* IDBKeyRange::Bound(v8::Local<v8::Value> lower,
                                v8::Local<v8::Value> upper,
                                bool lower_open,
                                bool upper_open,
                                bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  IDBKey lower_key;
  if (!CreateValidIDBKeyFromValue(isolate, lower, exception_state,
                                  &lower_key)) {
    return nullptr;
  }
  IDBKey upper_key;
  if (!CreateValidIDBKeyFromValue(isolate, upper, exception_state,
                                  &upper_key)) {
    return nullptr;
  }

  if (upper_key.IsLessThan(lower_key)) {
    exception_state.ThrowDOMException(
        "DataError", "The lower key is greater than the upper key.");
    return nullptr;
  }
  if (upper_key.Equals(lower_key) && (lower_open || upper_open)) {
    exception_state.ThrowDOMException(
        "DataError",
        "The lower key and upper key are equal and one of the bounds is open.");
    return nullptr;
  }

  return MakeRange(std::move(lower_key), std::move(upper_key), lower_open,
                   upper_open);
}

bool IDBKeyRange::Includes(v8::Local<v8::Value> key,
                           bindings::ExceptionState& exception_state) const {
  IDBKey converted;
  if (!CreateValidIDBKeyFromValue(v8::Isolate::GetCurrent(), key,
                                  exception_state, &converted)) {
    return false;
  }
  // An absent bound is None, which Contains() passes over, as Blink's
  // includes() does a null one.
  return range_.Contains(converted);
}

}  // namespace mitto
