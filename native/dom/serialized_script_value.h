// A value as the HTML Standard's StructuredSerialize() writes it, which
// StructuredDeserialize() reads back as a copy: what Blink's
// SerializedScriptValue holds of a mark's or a measure's detail. It is V8's
// own serializer, as Blink's is beneath it, and it refuses every object that
// wraps a C++ object, as Blink's refuses those it has no way to copy, which is
// all of the Runtime's for now.
//
// Upstream: third_party/blink/renderer/bindings/core/v8/serialization/serialized_script_value.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/serialization/v8_script_value_serializer.cc

#ifndef MITTO_DOM_SERIALIZED_SCRIPT_VALUE_H_
#define MITTO_DOM_SERIALIZED_SCRIPT_VALUE_H_

#include <stdint.h>

#include <optional>
#include <vector>

#include "v8/include/v8-forward.h"

namespace mitto {

class SerializedScriptValue {
 public:
  // Serializes `value`, or throws on the isolate and returns nullopt: a
  // DataCloneError for what cannot be copied, or what a page's getter threw.
  static std::optional<SerializedScriptValue> Serialize(
      v8::Isolate* isolate,
      v8::Local<v8::Value> value);

  SerializedScriptValue(const SerializedScriptValue&);
  SerializedScriptValue& operator=(const SerializedScriptValue&);
  SerializedScriptValue(SerializedScriptValue&&);
  SerializedScriptValue& operator=(SerializedScriptValue&&);
  ~SerializedScriptValue();

  // A new copy of the value, in the isolate's current context, or null if it
  // cannot be read back, as Blink gives null then.
  v8::Local<v8::Value> Deserialize(v8::Isolate* isolate) const;

 private:
  explicit SerializedScriptValue(std::vector<uint8_t> data);

  std::vector<uint8_t> data_;
};

}  // namespace mitto

#endif  // MITTO_DOM_SERIALIZED_SCRIPT_VALUE_H_
