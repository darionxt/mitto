#include "mitto/dom/serialized_script_value.h"

#include <stdlib.h>

#include <string>
#include <utility>

#include "base/compiler_specific.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-sandbox.h"
#include "v8/include/v8-value-serializer.h"

namespace mitto {
namespace {

void ThrowDataCloneError(v8::Isolate* isolate, std::string_view message) {
  const bindings::PerIsolateData::DOMExceptionHooks& hooks =
      bindings::PerIsolateData::From(isolate)->dom_exception_hooks();
  isolate->ThrowException(hooks.create(isolate, "DataCloneError", message));
}

// V8ScriptValueSerializer's delegate, for a Runtime that has no object which
// a structured clone copies.
class SerializerDelegate final : public v8::ValueSerializer::Delegate {
 public:
  explicit SerializerDelegate(v8::Isolate* isolate) : isolate_(isolate) {}
  ~SerializerDelegate() override = default;

  void ThrowDataCloneError(v8::Local<v8::String> message) override {
    std::u16string text;
    // A V8 string converts without running a script.
    bindings::FromV8String(isolate_, message, &text);
    mitto::ThrowDataCloneError(isolate_, base::UTF16ToUTF8(text));
  }

  bool HasCustomHostObject(v8::Isolate* isolate) override { return true; }

  v8::Maybe<bool> IsHostObject(v8::Isolate* isolate,
                               v8::Local<v8::Object> object) override {
    return v8::Just(object->IsApiWrapper());
  }

  v8::Maybe<bool> WriteHostObject(v8::Isolate* isolate,
                                  v8::Local<v8::Object> object) override {
    // V8ScriptValueSerializer::WriteHostObject(), whose WriteDOMObject() knows
    // no object of the Runtime's.
    auto* const wrappable = v8::Object::Unwrap<bindings::ScriptWrappable>(
        isolate, object, v8::kAnyCppHeapPointer);
    if (!wrappable) {
      mitto::ThrowDataCloneError(isolate, "An object could not be cloned.");
      return v8::Nothing<bool>();
    }
    mitto::ThrowDataCloneError(
        isolate, base::StrCat({wrappable->GetWrapperTypeInfo()->interface_name,
                               " object could not be cloned."}));
    return v8::Nothing<bool>();
  }

  void* ReallocateBufferMemory(void* old_buffer,
                               size_t size,
                               size_t* actual_size) override {
    *actual_size = size;
    return realloc(old_buffer, size);
  }

  void FreeBufferMemory(void* buffer) override { free(buffer); }

 private:
  const raw_ptr<v8::Isolate> isolate_;
};

}  // namespace

// static
std::optional<SerializedScriptValue> SerializedScriptValue::Serialize(
    v8::Isolate* isolate,
    v8::Local<v8::Value> value) {
  SerializerDelegate delegate(isolate);
  v8::ValueSerializer serializer(isolate, &delegate);
  serializer.WriteHeader();
  if (serializer.WriteValue(isolate->GetCurrentContext(), value).IsNothing()) {
    return std::nullopt;
  }
  const std::pair<uint8_t*, size_t> buffer = serializer.Release();
  // SAFETY: Release() gives a buffer of that size, which the delegate made.
  std::vector<uint8_t> data(buffer.first,
                            UNSAFE_BUFFERS(buffer.first + buffer.second));
  free(buffer.first);
  return SerializedScriptValue(std::move(data));
}

SerializedScriptValue::SerializedScriptValue(std::vector<uint8_t> data)
    : data_(std::move(data)) {}

SerializedScriptValue::SerializedScriptValue(const SerializedScriptValue&) =
    default;
SerializedScriptValue& SerializedScriptValue::operator=(
    const SerializedScriptValue&) = default;
SerializedScriptValue::SerializedScriptValue(SerializedScriptValue&&) = default;
SerializedScriptValue& SerializedScriptValue::operator=(
    SerializedScriptValue&&) = default;
SerializedScriptValue::~SerializedScriptValue() = default;

v8::Local<v8::Value> SerializedScriptValue::Deserialize(
    v8::Isolate* isolate) const {
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::ValueDeserializer deserializer(isolate, data_.data(), data_.size());
  v8::Local<v8::Value> value;
  // SerializedScriptValue::Deserialize() gives null for what it cannot read.
  if (!deserializer.ReadHeader(context).FromMaybe(false) ||
      !deserializer.ReadValue(context).ToLocal(&value)) {
    return v8::Null(isolate);
  }
  return value;
}

}  // namespace mitto
