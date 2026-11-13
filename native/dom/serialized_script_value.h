// A value as the HTML Standard's StructuredSerialize() writes it, which
// StructuredDeserialize() reads back as a copy: what Blink's
// SerializedScriptValue holds. It is V8's own serializer, as Blink's is
// beneath it, inside Blink's envelope, with the objects of the Runtime's that
// Blink copies or transfers: transferred ArrayBuffers and MessagePorts,
// DOMRect, DOMRectReadOnly, DOMException and QuotaExceededError, and
// WebAssembly modules. An object of an interface that Blink copies but the
// Runtime has no class for, such as DOMPoint or Blob, comes back as an object
// of that interface with nothing behind it, as a Known Gap's. Every other
// object that wraps a C++ object is refused with Chrome's DataCloneError.
//
// Upstream: third_party/blink/renderer/bindings/core/v8/serialization/serialized_script_value.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/serialization/v8_script_value_serializer.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/serialization/v8_script_value_deserializer.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/serialization/trailer_writer.cc

#ifndef MITTO_DOM_SERIALIZED_SCRIPT_VALUE_H_
#define MITTO_DOM_SERIALIZED_SCRIPT_VALUE_H_

#include <stdint.h>

#include <memory>
#include <optional>
#include <vector>

#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-wasm.h"

namespace mitto {

class MessagePort;

// What a serialization transfers, as Blink's Transferables: the ArrayBuffers
// and MessagePorts of a transfer list, each in the order of the list.
struct Transferables {
  Transferables();
  Transferables(const Transferables&) = delete;
  Transferables& operator=(const Transferables&) = delete;
  ~Transferables();

  std::vector<v8::Global<v8::ArrayBuffer>> array_buffers;
  std::vector<cppgc::Persistent<MessagePort>> message_ports;
};

class SerializedScriptValue {
 public:
  // Sorts the objects of a transfer list into `transferables`, as
  // SerializedScriptValue::ExtractTransferables() does, or throws Chrome's
  // DataCloneError on the isolate for a duplicate or for an object that the
  // Runtime does not transfer, and returns false.
  static bool ExtractTransferables(
      v8::Isolate* isolate,
      const std::vector<v8::Local<v8::Object>>& transfer,
      Transferables& transferables);

  // Serializes `value`, transferring the ArrayBuffers of `transferables`,
  // which it detaches, and writing its MessagePorts by their index. Or throws
  // on the isolate and returns nullopt: a DataCloneError for what cannot be
  // copied, or what a page's getter threw.
  static std::optional<SerializedScriptValue> Serialize(
      v8::Isolate* isolate,
      v8::Local<v8::Value> value,
      Transferables* transferables = nullptr);

  // A value that came from another process: its bytes alone, as Chrome's
  // CloneableMessage carries them, without the WebAssembly modules, which
  // stay in their agent cluster.
  static SerializedScriptValue FromWireData(std::vector<uint8_t> data);

  SerializedScriptValue(const SerializedScriptValue&);
  SerializedScriptValue& operator=(const SerializedScriptValue&);
  SerializedScriptValue(SerializedScriptValue&&);
  SerializedScriptValue& operator=(SerializedScriptValue&&);
  ~SerializedScriptValue();

  // A new copy of the value, in the isolate's current context, with `ports`
  // for the MessagePorts it was written with, or null if it cannot be read
  // back, as Blink gives null then. The ArrayBuffers it transferred go to the
  // first copy, which is the only one that a structured clone or a message
  // makes.
  v8::Local<v8::Value> Deserialize(
      v8::Isolate* isolate,
      const std::vector<MessagePort*>& ports = {}) const;

  // The bytes, which another process reads with FromWireData().
  const std::vector<uint8_t>& wire_data() const { return data_; }

  // Whether the value holds what only its own agent cluster can read: a
  // WebAssembly module, as SerializedScriptValue::IsLockedToAgentCluster()
  // says. The Runtime refuses SharedArrayBuffers before.
  bool IsLockedToAgentCluster() const { return !!wasm_modules_; }

 private:
  SerializedScriptValue();

  std::vector<uint8_t> data_;
  std::vector<std::shared_ptr<v8::BackingStore>> array_buffers_;
  // None if the value holds no module. CompiledWasmModule cannot be assigned,
  // so copies share the list.
  std::shared_ptr<const std::vector<v8::CompiledWasmModule>> wasm_modules_;
};

}  // namespace mitto

#endif  // MITTO_DOM_SERIALIZED_SCRIPT_VALUE_H_
