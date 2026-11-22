// A value as the HTML Standard's StructuredSerialize() writes it, which
// StructuredDeserialize() reads back as a copy: what Blink's
// SerializedScriptValue holds. It is V8's own serializer, as Blink's is
// beneath it, inside Blink's envelope, with the objects of the Runtime's that
// Blink copies or transfers: transferred ArrayBuffers, MessagePorts and
// streams, Blobs and Files, DOMRect, DOMRectReadOnly, DOMException and
// QuotaExceededError, WebAssembly modules, and SharedArrayBuffers from a
// context that may post them, whose memory the copy shares. A transferred
// stream goes as Blink's does, over a MessagePort of its own whose other end
// the value carries. A Blob goes by the UUID of its bytes, which the value
// carries beside its own bytes, as Blink's carries the blobs' handles. An
// object of an interface that Blink copies but the Runtime has no class for,
// such as DOMPoint or FileList, comes back as an object of that interface with
// nothing behind it, as a Known Gap's. Every other object that wraps a C++
// object is refused with Chrome's DataCloneError.
//
// Upstream: third_party/blink/renderer/bindings/core/v8/serialization/serialized_script_value.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/serialization/v8_script_value_serializer.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/serialization/v8_script_value_deserializer.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/serialization/trailer_writer.cc

#ifndef MITTO_DOM_SERIALIZED_SCRIPT_VALUE_H_
#define MITTO_DOM_SERIALIZED_SCRIPT_VALUE_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "mitto/dom/blob_data.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-wasm.h"

namespace mitto {

class MessagePort;
class ReadableStream;
class TransformStream;
class WritableStream;

// What a serialization transfers, as Blink's Transferables: the ArrayBuffers,
// MessagePorts and streams of a transfer list, each in the order of the list.
struct Transferables {
  Transferables();
  Transferables(const Transferables&) = delete;
  Transferables& operator=(const Transferables&) = delete;
  ~Transferables();

  std::vector<v8::Global<v8::ArrayBuffer>> array_buffers;
  std::vector<cppgc::Persistent<MessagePort>> message_ports;
  std::vector<cppgc::Persistent<ReadableStream>> readable_streams;
  std::vector<cppgc::Persistent<WritableStream>> writable_streams;
  std::vector<cppgc::Persistent<TransformStream>> transform_streams;
};

// The blobs that a value carries, by the UUIDs that its bytes name them by,
// as Blink's BlobDataHandleMap.
using BlobDataHandleMap = std::map<std::string, scoped_refptr<BlobDataHandle>>;

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
  // which it detaches, writing its MessagePorts by their index, and
  // transferring its streams, each over a new pair of ports. Or throws on the
  // isolate and returns nullopt: a DataCloneError for what cannot be copied,
  // or what a page's getter threw.
  static std::optional<SerializedScriptValue> Serialize(
      v8::Isolate* isolate,
      v8::Local<v8::Value> value,
      Transferables* transferables = nullptr);

  // Serializes `value` as IndexedDB stores it, with Blink's
  // SerializeOptions::kForStorage: a SharedArrayBuffer or a WebAssembly
  // module throws a DataCloneError where the serializer meets it, before the
  // getters after it run.
  static std::optional<SerializedScriptValue> SerializeForStorage(
      v8::Isolate* isolate,
      v8::Local<v8::Value> value);

  // A value that went as its bytes, as Chrome's CloneableMessage carries
  // them, without the WebAssembly modules and the SharedArrayBuffers, which
  // stay in their agent cluster. One from a context of the same Page has
  // `blob_data_handles`, the blobs it carries, as Chrome's browser hands them
  // over. One from another process has none, so its blobs read as empty: a
  // Known Gap.
  static SerializedScriptValue FromWireData(
      std::vector<uint8_t> data,
      BlobDataHandleMap blob_data_handles = BlobDataHandleMap());

  SerializedScriptValue(const SerializedScriptValue&);
  SerializedScriptValue& operator=(const SerializedScriptValue&);
  SerializedScriptValue(SerializedScriptValue&&);
  SerializedScriptValue& operator=(SerializedScriptValue&&);
  ~SerializedScriptValue();

  // A new copy of the value, in the isolate's current context, with `ports`
  // for the MessagePorts it was written with, or null if it cannot be read
  // back, as Blink gives null then. The ArrayBuffers and streams it
  // transferred go to the first copy, which is the only one that a structured
  // clone or a message makes.
  v8::Local<v8::Value> Deserialize(
      v8::Isolate* isolate,
      const std::vector<MessagePort*>& ports = {}) const;

  // The bytes, which another process reads with FromWireData().
  const std::vector<uint8_t>& wire_data() const { return data_; }
  // The blobs that the bytes name, by their UUIDs.
  const BlobDataHandleMap& blob_data_handles() const {
    return blob_data_handles_;
  }

  // Whether the value holds what only its own agent cluster can read: a
  // WebAssembly module or a SharedArrayBuffer, as
  // SerializedScriptValue::IsLockedToAgentCluster() says.
  bool IsLockedToAgentCluster() const {
    return !!wasm_modules_ || !shared_array_buffers_.empty();
  }

 private:
  SerializedScriptValue();

  static std::optional<SerializedScriptValue> SerializeWith(
      v8::Isolate* isolate,
      v8::Local<v8::Value> value,
      Transferables* transferables,
      bool for_storage);

  std::vector<uint8_t> data_;
  std::vector<std::shared_ptr<v8::BackingStore>> array_buffers_;
  BlobDataHandleMap blob_data_handles_;
  // The memory of the SharedArrayBuffers that the value holds, which each
  // copy shares, as Blink's shared_array_buffers_contents_.
  std::vector<std::shared_ptr<v8::BackingStore>> shared_array_buffers_;
  // None if the value holds no module. CompiledWasmModule cannot be assigned,
  // so copies share the list.
  std::shared_ptr<const std::vector<v8::CompiledWasmModule>> wasm_modules_;
  // The other ends of the ports of the streams it transferred, as Blink's
  // streams_, or none. Copies share them, and the first copy read takes them.
  struct StreamChannels;
  std::shared_ptr<StreamChannels> streams_;
};

}  // namespace mitto

#endif  // MITTO_DOM_SERIALIZED_SCRIPT_VALUE_H_
