// IndexedDB's IDBKeyRange: a range of keys, with a lower and an upper bound,
// either of which may be absent and each of which may be open, that a page
// makes with only(), lowerBound(), upperBound() or bound(), and that the
// methods of object stores, indexes and cursors read, as a key too. A range
// travels to the Session as an IndexedDbKeyRange, in which an absent bound is
// None, and so does no range, with both bounds None and closed.
//
// Blink keeps one key for both bounds of a range of one key; this keeps both.
//
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_key_range.cc

#ifndef MITTO_DOM_IDB_KEY_RANGE_H_
#define MITTO_DOM_IDB_KEY_RANGE_H_

#include <optional>

#include "mitto/bindings/script_wrappable.h"
#include "mitto/indexed_db_key.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-local-handle.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class IDBKeyRange final : public bindings::ScriptWrappable {
 public:
  // A range of `range`'s bounds, each of which is valid, or None for one that
  // is absent, which is open.
  explicit IDBKeyRange(IndexedDbKeyRange range);
  ~IDBKeyRange() override;

  // Blink's FromScriptValue(): the range of `value` into `*range`: nullopt for
  // null or undefined, a range's own, or the range of the one key that the
  // value converts to, or Chrome's DataError for a value that does not convert
  // to a valid key. Returns false if anything threw.
  static bool FromScriptValue(v8::Isolate* isolate,
                              v8::Local<v8::Value> value,
                              bindings::ExceptionState& exception_state,
                              std::optional<IndexedDbKeyRange>* range);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The range, as it travels to the Session.
  const IndexedDbKeyRange& Range() const { return range_; }

  // The members of the interface.
  v8::Local<v8::Value> LowerValue() const;
  v8::Local<v8::Value> UpperValue() const;
  bool LowerOpen() const { return range_.lower_open; }
  bool UpperOpen() const { return range_.upper_open; }
  static IDBKeyRange* Only(v8::Local<v8::Value> value,
                           bindings::ExceptionState& exception_state);
  static IDBKeyRange* LowerBound(v8::Local<v8::Value> bound,
                                 bool open,
                                 bindings::ExceptionState& exception_state);
  static IDBKeyRange* UpperBound(v8::Local<v8::Value> bound,
                                 bool open,
                                 bindings::ExceptionState& exception_state);
  static IDBKeyRange* Bound(v8::Local<v8::Value> lower,
                            v8::Local<v8::Value> upper,
                            bool lower_open,
                            bool upper_open,
                            bindings::ExceptionState& exception_state);
  bool Includes(v8::Local<v8::Value> key,
                bindings::ExceptionState& exception_state) const;

 private:
  const IndexedDbKeyRange range_;
};

}  // namespace mitto

#endif  // MITTO_DOM_IDB_KEY_RANGE_H_
