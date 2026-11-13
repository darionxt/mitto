#include "mitto/dom/serialized_script_value.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/memory/raw_ref.h"
#include "base/no_destructor.h"
#include "base/numerics/byte_conversions.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/idl_member_installer.h"
#include "mitto/bindings/interfaces_without_class.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_dom_exception.h"
#include "mitto/bindings/v8_dom_rect.h"
#include "mitto/bindings/v8_dom_rect_read_only.h"
#include "mitto/bindings/v8_message_port.h"
#include "mitto/bindings/v8_quota_exceeded_error.h"
#include "mitto/dom/dom_exception.h"
#include "mitto/dom/dom_rect.h"
#include "mitto/dom/dom_rect_read_only.h"
#include "mitto/dom/message_port.h"
#include "mitto/dom/quota_exceeded_error.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-value-serializer.h"

namespace mitto {
namespace {

// SerializationTag's values that the Runtime writes.
constexpr uint8_t kVersionTag = 0xFF;
constexpr uint8_t kTrailerOffsetTag = 0xFE;
constexpr uint8_t kTrailerRequiresInterfacesTag = 0xA0;
constexpr uint8_t kMessagePortTag = 'M';
constexpr uint8_t kDOMRectTag = 'E';
constexpr uint8_t kDOMRectReadOnlyTag = 'R';
constexpr uint8_t kDOMExceptionTag = 'x';
constexpr uint8_t kQuotaExceededErrorTag = 'q';
// The Runtime's own, which Blink does not use: an object of one of
// kCopiedWithoutClass, by its index there.
constexpr uint8_t kObjectWithoutClassTag = '?';

// SerializedScriptValue::kWireFormatVersion.
constexpr uint32_t kWireFormatVersion = 21;
// Where the trailer's offset goes: after the version tag, the version and the
// trailer offset tag.
constexpr size_t kTrailerOffsetPosition = 3;
constexpr size_t kTrailerOffsetSize = sizeof(uint64_t) + sizeof(uint32_t);

// The interfaces that Blink's V8ScriptValueSerializer copies whose objects a
// Runtime without their class has with nothing behind them, each with the tag
// that Blink requires the receiving context to expose, each before those it
// inherits from. Its copy is another such object. Giving one of them a class
// means writing what Blink writes.
struct CopiedWithoutClass {
  // RAW_PTR_EXCLUSION: #global-scope, as WrapperTypeInfo's own.
  RAW_PTR_EXCLUSION const bindings::WrapperTypeInfo* info;
  uint8_t blink_tag;
};
const std::array<CopiedWithoutClass, 10>& CopiedWithoutClassInterfaces() {
  static const std::array<CopiedWithoutClass, 10> interfaces = {{
      {&bindings::V8File::kWrapperTypeInfo, 'f'},
      {&bindings::V8Blob::kWrapperTypeInfo, 'b'},
      {&bindings::V8FileList::kWrapperTypeInfo, 'l'},
      {&bindings::V8ImageBitmap::kWrapperTypeInfo, 'g'},
      {&bindings::V8ImageData::kWrapperTypeInfo, '#'},
      {&bindings::V8DOMPoint::kWrapperTypeInfo, 'Q'},
      {&bindings::V8DOMPointReadOnly::kWrapperTypeInfo, 'W'},
      {&bindings::V8DOMQuad::kWrapperTypeInfo, 'T'},
      {&bindings::V8DOMMatrix::kWrapperTypeInfo, 'Y'},
      {&bindings::V8DOMMatrixReadOnly::kWrapperTypeInfo, 'U'},
  }};
  return interfaces;
}

void ThrowDOMException(v8::Isolate* isolate,
                       std::string_view name,
                       std::string_view message) {
  const bindings::PerIsolateData::DOMExceptionHooks& hooks =
      bindings::PerIsolateData::From(isolate)->dom_exception_hooks();
  isolate->ThrowException(hooks.create(isolate, name, message));
}

void ThrowDataCloneError(v8::Isolate* isolate, std::string_view message) {
  ThrowDOMException(isolate, "DataCloneError", message);
}

std::string IndexText(size_t index) {
  return base::NumberToString(index);
}

// V8ScriptValueSerializer.
class Serializer final : public v8::ValueSerializer::Delegate {
 public:
  Serializer(v8::Isolate* isolate, Transferables* transferables)
      : isolate_(isolate),
        serializer_(isolate, this),
        transferables_(transferables) {}
  ~Serializer() override = default;

  // V8ScriptValueSerializer::Serialize(): the bytes, the transferred
  // ArrayBuffers' contents and the WebAssembly modules, or false after
  // throwing.
  bool Serialize(v8::Local<v8::Value> value,
                 std::vector<uint8_t>& data,
                 std::vector<std::shared_ptr<v8::BackingStore>>& array_buffers,
                 std::vector<v8::CompiledWasmModule>& wasm_modules) {
    // PrepareTransfer().
    if (transferables_) {
      for (size_t i = 0; i < transferables_->array_buffers.size(); ++i) {
        serializer_.TransferArrayBuffer(
            base::checked_cast<uint32_t>(i),
            transferables_->array_buffers[i].Get(isolate_));
      }
    }
    // The envelope, whose trailer offset is filled in at the end.
    WriteTag(kVersionTag);
    serializer_.WriteUint32(kWireFormatVersion);
    WriteTag(kTrailerOffsetTag);
    static constexpr uint8_t kZeroOffset[kTrailerOffsetSize] = {};
    serializer_.WriteRawBytes(kZeroOffset, sizeof(kZeroOffset));
    serializer_.WriteHeader();

    const v8::Local<v8::Context> context = isolate_->GetCurrentContext();
    if (serializer_.WriteValue(context, value).IsNothing()) {
      return false;
    }
    // FinalizeTransfer(): TransferArrayBufferContents().
    if (transferables_) {
      std::vector<v8::Local<v8::ArrayBuffer>> buffers;
      for (const v8::Global<v8::ArrayBuffer>& buffer :
           transferables_->array_buffers) {
        buffers.push_back(buffer.Get(isolate_));
      }
      for (size_t i = 0; i < buffers.size(); ++i) {
        if (buffers[i]->WasDetached()) {
          mitto::ThrowDataCloneError(
              isolate_, base::StrCat({"ArrayBuffer at index ", IndexText(i),
                                      " is already detached."}));
          return false;
        }
      }
      for (size_t i = 0; i < buffers.size(); ++i) {
        if (!buffers[i]->IsDetachable()) {
          bindings::ThrowTypeError(
              isolate_, base::StrCat({"ArrayBuffer at index ", IndexText(i),
                                      " is not detachable and could not be "
                                      "transferred."}));
          return false;
        }
        std::shared_ptr<v8::BackingStore> contents =
            buffers[i]->GetBackingStore();
        if (buffers[i]->Detach(v8::Local<v8::Value>()).IsNothing()) {
          return false;
        }
        array_buffers.push_back(std::move(contents));
      }
    }
    // A Page is never cross-origin isolated, so the SharedArrayBuffers that
    // the value holds are refused once the rest is done, as Blink's
    // CheckSharedArrayBufferTransferAllowedAndReport() refuses them.
    if (has_shared_array_buffer_) {
      mitto::ThrowDataCloneError(
          isolate_,
          "SharedArrayBuffer transfer requires self.crossOriginIsolated.");
      return false;
    }
    // The trailer, which names the interfaces that the value needs.
    std::vector<uint8_t> trailer;
    if (!required_interfaces_.empty()) {
      trailer.push_back(kTrailerRequiresInterfacesTag);
      const auto count = base::U32ToBigEndian(
          base::checked_cast<uint32_t>(required_interfaces_.size()));
      trailer.insert(trailer.end(), count.begin(), count.end());
      trailer.insert(trailer.end(), required_interfaces_.begin(),
                     required_interfaces_.end());
      serializer_.WriteRawBytes(trailer.data(), trailer.size());
    }
    const std::pair<uint8_t*, size_t> buffer = serializer_.Release();
    // SAFETY: Release() gives a buffer of that size, which this made.
    data.assign(buffer.first, UNSAFE_BUFFERS(buffer.first + buffer.second));
    free(buffer.first);
    if (!trailer.empty()) {
      base::span<uint8_t> offset =
          base::span(data).subspan(kTrailerOffsetPosition, kTrailerOffsetSize);
      offset.first<8u>().copy_from(
          base::U64ToBigEndian(data.size() - trailer.size()));
      offset.last<4u>().copy_from(
          base::U32ToBigEndian(base::checked_cast<uint32_t>(trailer.size())));
    }
    for (v8::CompiledWasmModule& module : wasm_modules_) {
      wasm_modules.push_back(std::move(module));
    }
    return true;
  }

  // v8::ValueSerializer::Delegate:
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
    auto* const wrappable = v8::Object::Unwrap<bindings::ScriptWrappable>(
        isolate, object, v8::kAnyCppHeapPointer);
    if (!wrappable) {
      // An object of an interface without a class, with nothing behind it.
      if (WriteObjectWithoutClass(object)) {
        return v8::Just(true);
      }
      std::u16string name;
      bindings::FromV8String(isolate_, object->GetConstructorName(), &name);
      mitto::ThrowDataCloneError(
          isolate, name.empty()
                       ? "An object could not be cloned."
                       : base::StrCat({base::UTF16ToUTF8(name),
                                       " object could not be cloned."}));
      return v8::Nothing<bool>();
    }
    if (WriteDOMObject(*wrappable)) {
      return v8::Just(true);
    }
    if (!thrown_) {
      mitto::ThrowDataCloneError(
          isolate,
          base::StrCat({wrappable->GetWrapperTypeInfo()->interface_name,
                        " object could not be cloned."}));
    }
    return v8::Nothing<bool>();
  }

  v8::Maybe<uint32_t> GetSharedArrayBufferId(
      v8::Isolate* isolate,
      v8::Local<v8::SharedArrayBuffer> shared_array_buffer) override {
    // Refused once the value is written, as Blink checks the transfer then.
    has_shared_array_buffer_ = true;
    return v8::Just<uint32_t>(0);
  }

  v8::Maybe<uint32_t> GetWasmModuleTransferId(
      v8::Isolate* isolate,
      v8::Local<v8::WasmModuleObject> module) override {
    // Options::kTransfer, as every serialization of the Runtime's has.
    wasm_modules_.push_back(module->GetCompiledModule());
    return v8::Just(base::checked_cast<uint32_t>(wasm_modules_.size() - 1));
  }

  void* ReallocateBufferMemory(void* old_buffer,
                               size_t size,
                               size_t* actual_size) override {
    *actual_size = size;
    return realloc(old_buffer, size);
  }

  void FreeBufferMemory(void* buffer) override { free(buffer); }

 private:
  void WriteTag(uint8_t tag) { serializer_.WriteRawBytes(&tag, 1); }

  void WriteAndRequireInterfaceTag(uint8_t tag) {
    WriteTag(tag);
    RequireInterface(tag);
  }

  void RequireInterface(uint8_t tag) {
    if (std::find(required_interfaces_.begin(), required_interfaces_.end(),
                  tag) == required_interfaces_.end()) {
      required_interfaces_.push_back(tag);
    }
  }

  void WriteUTF8String(const std::u16string& text) {
    const std::string utf8 = base::UTF16ToUTF8(text);
    serializer_.WriteUint32(base::checked_cast<uint32_t>(utf8.size()));
    serializer_.WriteRawBytes(utf8.data(), utf8.size());
  }

  // V8ScriptValueSerializer::WriteDOMObject(), for the objects of the
  // Runtime's: whether it wrote `wrappable`. It throws why not where Blink
  // does, and sets `thrown_`.
  bool WriteDOMObject(bindings::ScriptWrappable& wrappable) {
    const bindings::WrapperTypeInfo* const info =
        wrappable.GetWrapperTypeInfo();
    // Blink's TypeDispatcher::ToMostDerived(): the object's own interface.
    if (info == &bindings::V8DOMRect::kWrapperTypeInfo ||
        info == &bindings::V8DOMRectReadOnly::kWrapperTypeInfo) {
      const auto& rect = static_cast<const DOMRectReadOnly&>(wrappable);
      WriteAndRequireInterfaceTag(info == &bindings::V8DOMRect::kWrapperTypeInfo
                                      ? kDOMRectTag
                                      : kDOMRectReadOnlyTag);
      serializer_.WriteDouble(rect.X());
      serializer_.WriteDouble(rect.Y());
      serializer_.WriteDouble(rect.Width());
      serializer_.WriteDouble(rect.Height());
      return true;
    }
    if (info == &bindings::V8MessagePort::kWrapperTypeInfo) {
      auto& port = static_cast<MessagePort&>(wrappable);
      if (transferables_) {
        for (size_t i = 0; i < transferables_->message_ports.size(); ++i) {
          if (transferables_->message_ports[i].Get() == &port) {
            WriteAndRequireInterfaceTag(kMessagePortTag);
            serializer_.WriteUint32(base::checked_cast<uint32_t>(i));
            return true;
          }
        }
      }
      mitto::ThrowDataCloneError(isolate_,
                                 "A MessagePort could not be cloned because "
                                 "it was not transferred.");
      thrown_ = true;
      return false;
    }
    if (info == &bindings::V8DOMException::kWrapperTypeInfo) {
      const auto& exception = static_cast<const DOMException&>(wrappable);
      WriteAndRequireInterfaceTag(kDOMExceptionTag);
      WriteUTF8String(exception.Name());
      WriteUTF8String(exception.Message());
      // The stack, which Blink keeps a place for and writes empty.
      WriteUTF8String(std::u16string());
      return true;
    }
    if (info == &bindings::V8QuotaExceededError::kWrapperTypeInfo) {
      const auto& error = static_cast<const QuotaExceededError&>(wrappable);
      WriteAndRequireInterfaceTag(kQuotaExceededErrorTag);
      WriteUTF8String(error.Message());
      WriteUTF8String(std::u16string());
      serializer_.WriteUint32(error.Quota().has_value() ? 1 : 0);
      serializer_.WriteDouble(error.Quota().value_or(0));
      serializer_.WriteUint32(error.Requested().has_value() ? 1 : 0);
      serializer_.WriteDouble(error.Requested().value_or(0));
      return true;
    }
    if (info->table) {
      const auto& interfaces = CopiedWithoutClassInterfaces();
      for (size_t i = 0; i < interfaces.size(); ++i) {
        if (interfaces[i].info == info) {
          WriteObjectWithoutClass(i);
          return true;
        }
      }
    }
    return false;
  }

  // Whether `object`, which wraps nothing, is of one of the interfaces that
  // Blink copies, which it then wrote.
  bool WriteObjectWithoutClass(v8::Local<v8::Object> object) {
    const bindings::PerIsolateData* const data =
        bindings::PerIsolateData::From(isolate_);
    const auto& interfaces = CopiedWithoutClassInterfaces();
    for (size_t i = 0; i < interfaces.size(); ++i) {
      if (data->HasInstance(isolate_, *interfaces[i].info, object)) {
        WriteObjectWithoutClass(i);
        return true;
      }
    }
    return false;
  }

  void WriteObjectWithoutClass(size_t index) {
    WriteTag(kObjectWithoutClassTag);
    RequireInterface(CopiedWithoutClassInterfaces()[index].blink_tag);
    serializer_.WriteUint32(base::checked_cast<uint32_t>(index));
  }

  const raw_ptr<v8::Isolate> isolate_;
  v8::ValueSerializer serializer_;
  const raw_ptr<Transferables> transferables_;
  std::vector<uint8_t> required_interfaces_;
  std::vector<v8::CompiledWasmModule> wasm_modules_;
  bool has_shared_array_buffer_ = false;
  bool thrown_ = false;
};

// The size of Blink's envelope at the start of `data`, or 0 if it has none:
// ReadVersionEnvelope().
size_t VersionEnvelopeSize(const std::vector<uint8_t>& data) {
  if (data.size() < 2 || data[0] != kVersionTag) {
    return 0;
  }
  // The version, a varint of up to five bytes.
  uint32_t version = 0;
  size_t i = 1;
  for (int shift = 0; i < data.size() && shift < 32; shift += 7) {
    const uint8_t byte = data[i++];
    version |= static_cast<uint32_t>(byte & 0x7F) << shift;
    if (!(byte & 0x80)) {
      break;
    }
  }
  if (version < kWireFormatVersion) {
    return 0;
  }
  if (i >= data.size() || data[i] != kTrailerOffsetTag) {
    return 0;
  }
  i += 1 + kTrailerOffsetSize;
  return i < data.size() ? i : 0;
}

// The tags of the interfaces that the trailer of `data` names, as
// TrailerReader reads them: those of the objects that the value holds.
std::vector<uint8_t> RequiredInterfaces(const std::vector<uint8_t>& data) {
  if (data.size() < kTrailerOffsetPosition + kTrailerOffsetSize) {
    return {};
  }
  const base::span<const uint8_t> offset_bytes =
      base::span(data).subspan(kTrailerOffsetPosition, kTrailerOffsetSize);
  const uint64_t offset = base::U64FromBigEndian(offset_bytes.first<8u>());
  const uint32_t size = base::U32FromBigEndian(offset_bytes.last<4u>());
  if (size < 5 || offset > data.size() || size > data.size() - offset) {
    return {};
  }
  const base::span<const uint8_t> trailer =
      base::span(data).subspan(static_cast<size_t>(offset), size);
  const uint32_t count = base::U32FromBigEndian(trailer.subspan<1u, 4u>());
  if (trailer[0] != kTrailerRequiresInterfacesTag ||
      count != trailer.size() - 5) {
    return {};
  }
  const base::span<const uint8_t> tags = trailer.subspan(5u);
  return std::vector<uint8_t>(tags.begin(), tags.end());
}

// The interface of the Runtime's that Blink's tag `tag` stands for, or null.
const bindings::WrapperTypeInfo* InterfaceOfTag(uint8_t tag) {
  switch (tag) {
    case kDOMRectTag:
      return &bindings::V8DOMRect::kWrapperTypeInfo;
    case kDOMRectReadOnlyTag:
      return &bindings::V8DOMRectReadOnly::kWrapperTypeInfo;
    case kDOMExceptionTag:
      return &bindings::V8DOMException::kWrapperTypeInfo;
    case kQuotaExceededErrorTag:
      return &bindings::V8QuotaExceededError::kWrapperTypeInfo;
    case kMessagePortTag:
      return &bindings::V8MessagePort::kWrapperTypeInfo;
    default:
      for (const CopiedWithoutClass& copied : CopiedWithoutClassInterfaces()) {
        if (copied.blink_tag == tag) {
          return copied.info;
        }
      }
      return nullptr;
  }
}

// V8ScriptValueDeserializer.
class Deserializer final : public v8::ValueDeserializer::Delegate {
 public:
  Deserializer(v8::Isolate* isolate,
               const std::vector<uint8_t>& data,
               const std::vector<MessagePort*>& ports,
               const std::vector<v8::CompiledWasmModule>& wasm_modules)
      : isolate_(isolate),
        deserializer_(isolate, data.data(), data.size(), this),
        data_(data),
        ports_(ports),
        wasm_modules_(wasm_modules) {}
  ~Deserializer() override = default;

  v8::MaybeLocal<v8::Value> Deserialize(
      const std::vector<std::shared_ptr<v8::BackingStore>>& array_buffers) {
    const v8::Local<v8::Context> context = isolate_->GetCurrentContext();
    const size_t envelope = VersionEnvelopeSize(*data_);
    if (envelope == 0) {
      return {};
    }
    const void* skipped = nullptr;
    if (!deserializer_.ReadRawBytes(envelope, &skipped) ||
        !deserializer_.ReadHeader(context).FromMaybe(false)) {
      return {};
    }
    // Transfer().
    for (size_t i = 0; i < array_buffers.size(); ++i) {
      deserializer_.TransferArrayBuffer(
          base::checked_cast<uint32_t>(i),
          v8::ArrayBuffer::New(isolate_, array_buffers[i]));
    }
    // V8 runs no script while it reads a host object, and making an interface
    // object, or finding the one the context made, does. So the context makes
    // those of the objects that the trailer names first, and each port gets
    // its wrapper.
    for (const uint8_t tag : RequiredInterfaces(*data_)) {
      if (const bindings::WrapperTypeInfo* const info = InterfaceOfTag(tag)) {
        std::ignore = bindings::InterfaceObject(context, *info);
        prepared_.push_back(info);
      }
    }
    for (MessagePort* const port : *ports_) {
      if (port) {
        port->ToV8(isolate_);
      }
    }
    return deserializer_.ReadValue(context);
  }

  // v8::ValueDeserializer::Delegate:
  v8::MaybeLocal<v8::Object> ReadHostObject(v8::Isolate* isolate) override {
    // What cannot be read makes the whole value null, as Blink's does after
    // its "Unable to deserialize cloned data.", which no page sees. Making
    // that DOMException here would run a script, which V8 does not allow.
    v8::Local<v8::Object> object;
    uint8_t tag = 0;
    if (!ReadTag(tag) || !ReadDOMObject(tag).ToLocal(&object)) {
      return {};
    }
    return object;
  }

  v8::MaybeLocal<v8::WasmModuleObject> GetWasmModuleFromId(
      v8::Isolate* isolate,
      uint32_t id) override {
    if (id < wasm_modules_->size()) {
      return v8::WasmModuleObject::FromCompiledModule(isolate,
                                                      (*wasm_modules_)[id]);
    }
    // A module that stayed in its agent cluster, as one that a
    // BroadcastChannel sent does.
    return {};
  }

  // The Runtime writes no SharedArrayBuffer, so one makes the value null.
  v8::MaybeLocal<v8::SharedArrayBuffer> GetSharedArrayBufferFromId(
      v8::Isolate* isolate,
      uint32_t id) override {
    return {};
  }

 private:
  bool ReadTag(uint8_t& tag) {
    const void* byte = nullptr;
    if (!deserializer_.ReadRawBytes(1, &byte)) {
      return false;
    }
    tag = *static_cast<const uint8_t*>(byte);
    return true;
  }

  bool ReadUTF8String(std::u16string& text) {
    uint32_t size = 0;
    const void* bytes = nullptr;
    if (!deserializer_.ReadUint32(&size) ||
        !deserializer_.ReadRawBytes(size, &bytes)) {
      return false;
    }
    return base::UTF8ToUTF16(static_cast<const char*>(bytes), size, &text);
  }

  // A new object of the interface `info`, whose interface object the context
  // made before V8 started to read, or nothing if the trailer did not name it.
  v8::MaybeLocal<v8::Object> NewInstance(
      const bindings::WrapperTypeInfo& info) {
    if (std::find(prepared_.begin(), prepared_.end(), &info) ==
        prepared_.end()) {
      return {};
    }
    return bindings::PerIsolateData::From(isolate_)
        ->InterfaceTemplate(isolate_, &info)
        ->InstanceTemplate()
        ->NewInstance(isolate_->GetCurrentContext());
  }

  // The wrapper of `object`, which V8 read, as ScriptWrappable::ToV8() makes
  // one.
  v8::MaybeLocal<v8::Object> NewWrapper(bindings::ScriptWrappable* object) {
    v8::Local<v8::Object> wrapper;
    if (!NewInstance(*object->GetWrapperTypeInfo()).ToLocal(&wrapper)) {
      return {};
    }
    object->AssociateWithWrapper(isolate_, wrapper);
    return wrapper;
  }

  // ReadDOMObject(), for the tags that the Runtime writes.
  v8::MaybeLocal<v8::Object> ReadDOMObject(uint8_t tag) {
    switch (tag) {
      case kDOMRectTag:
      case kDOMRectReadOnlyTag: {
        double x = 0;
        double y = 0;
        double width = 0;
        double height = 0;
        if (!deserializer_.ReadDouble(&x) || !deserializer_.ReadDouble(&y) ||
            !deserializer_.ReadDouble(&width) ||
            !deserializer_.ReadDouble(&height)) {
          return {};
        }
        if (tag == kDOMRectTag) {
          return NewWrapper(DOMRect::Create(x, y, width, height));
        }
        return NewWrapper(DOMRectReadOnly::Create(x, y, width, height));
      }
      case kMessagePortTag: {
        uint32_t index = 0;
        if (!deserializer_.ReadUint32(&index) || index >= ports_->size() ||
            !(*ports_)[index]) {
          return {};
        }
        // Its wrapper was made before V8 started to read.
        return (*ports_)[index]->ToV8(isolate_);
      }
      case kDOMExceptionTag: {
        std::u16string name;
        std::u16string message;
        std::u16string stack_unused;
        if (!ReadUTF8String(name) || !ReadUTF8String(message) ||
            !ReadUTF8String(stack_unused)) {
          return {};
        }
        return NewWrapper(DOMException::Create(message, name));
      }
      case kQuotaExceededErrorTag: {
        std::u16string message;
        std::u16string stack_unused;
        uint32_t has_quota = 0;
        uint32_t has_requested = 0;
        double quota = 0;
        double requested = 0;
        if (!ReadUTF8String(message) || !ReadUTF8String(stack_unused) ||
            !deserializer_.ReadUint32(&has_quota) ||
            !deserializer_.ReadDouble(&quota) ||
            !deserializer_.ReadUint32(&has_requested) ||
            !deserializer_.ReadDouble(&requested)) {
          return {};
        }
        return NewWrapper(QuotaExceededError::Create(
            message, has_quota ? std::make_optional(quota) : std::nullopt,
            has_requested ? std::make_optional(requested) : std::nullopt));
      }
      case kObjectWithoutClassTag: {
        uint32_t index = 0;
        const auto& interfaces = CopiedWithoutClassInterfaces();
        if (!deserializer_.ReadUint32(&index) || index >= interfaces.size()) {
          return {};
        }
        // An object of the interface with nothing behind it, as
        // NewObjectOfInterface() makes one, from the template alone.
        return NewInstance(*interfaces[index].info);
      }
      default:
        return {};
    }
  }

  const raw_ptr<v8::Isolate> isolate_;
  v8::ValueDeserializer deserializer_;
  const raw_ref<const std::vector<uint8_t>> data_;
  const raw_ref<const std::vector<MessagePort*>> ports_;
  const raw_ref<const std::vector<v8::CompiledWasmModule>> wasm_modules_;
  // The interfaces whose interface objects the context made before V8
  // started to read.
  std::vector<const bindings::WrapperTypeInfo*> prepared_;
};

}  // namespace

Transferables::Transferables() = default;
Transferables::~Transferables() = default;

// static
bool SerializedScriptValue::ExtractTransferables(
    v8::Isolate* isolate,
    const std::vector<v8::Local<v8::Object>>& transfer,
    Transferables& transferables) {
  for (size_t i = 0; i < transfer.size(); ++i) {
    const v8::Local<v8::Object> object = transfer[i];
    // V8ScriptValueSerializer::ExtractTransferable().
    if (MessagePort* const port =
            bindings::V8MessagePort::ToWrappable(isolate, object)) {
      for (const cppgc::Persistent<MessagePort>& earlier :
           transferables.message_ports) {
        if (earlier.Get() == port) {
          ThrowDataCloneError(
              isolate, base::StrCat({"Message port at index ", IndexText(i),
                                     " is a duplicate of an earlier port."}));
          return false;
        }
      }
      transferables.message_ports.emplace_back(port);
      continue;
    }
    if (object->IsArrayBuffer()) {
      const v8::Local<v8::ArrayBuffer> buffer = object.As<v8::ArrayBuffer>();
      for (const v8::Global<v8::ArrayBuffer>& earlier :
           transferables.array_buffers) {
        if (earlier == buffer) {
          ThrowDataCloneError(
              isolate,
              base::StrCat({"ArrayBuffer at index ", IndexText(i),
                            " is a duplicate of an earlier ArrayBuffer."}));
          return false;
        }
      }
      transferables.array_buffers.emplace_back(isolate, buffer);
      continue;
    }
    // ImageBitmap, OffscreenCanvas and the streams, which Blink transfers,
    // have no class in the Runtime, and so no object of theirs moves: a Known
    // Gap.
    ThrowDataCloneError(isolate,
                        base::StrCat({"Value at index ", IndexText(i),
                                      " does not have a transferable type."}));
    return false;
  }
  return true;
}

// static
std::optional<SerializedScriptValue> SerializedScriptValue::Serialize(
    v8::Isolate* isolate,
    v8::Local<v8::Value> value,
    Transferables* transferables) {
  SerializedScriptValue serialized;
  Serializer serializer(isolate, transferables);
  std::vector<v8::CompiledWasmModule> wasm_modules;
  if (!serializer.Serialize(value, serialized.data_, serialized.array_buffers_,
                            wasm_modules)) {
    return std::nullopt;
  }
  if (!wasm_modules.empty()) {
    serialized.wasm_modules_ =
        std::make_shared<const std::vector<v8::CompiledWasmModule>>(
            std::move(wasm_modules));
  }
  return serialized;
}

// static
SerializedScriptValue SerializedScriptValue::FromWireData(
    std::vector<uint8_t> data) {
  SerializedScriptValue serialized;
  serialized.data_ = std::move(data);
  return serialized;
}

SerializedScriptValue::SerializedScriptValue() = default;
SerializedScriptValue::SerializedScriptValue(const SerializedScriptValue&) =
    default;
SerializedScriptValue& SerializedScriptValue::operator=(
    const SerializedScriptValue&) = default;
SerializedScriptValue::SerializedScriptValue(SerializedScriptValue&&) = default;
SerializedScriptValue& SerializedScriptValue::operator=(
    SerializedScriptValue&&) = default;
SerializedScriptValue::~SerializedScriptValue() = default;

v8::Local<v8::Value> SerializedScriptValue::Deserialize(
    v8::Isolate* isolate,
    const std::vector<MessagePort*>& ports) const {
  v8::EscapableHandleScope handle_scope(isolate);
  // What fails to read gives null, and throws nothing at the page.
  v8::TryCatch try_catch(isolate);
  static const base::NoDestructor<std::vector<v8::CompiledWasmModule>> kNone;
  Deserializer deserializer(isolate, data_, ports,
                            wasm_modules_ ? *wasm_modules_ : *kNone);
  v8::Local<v8::Value> value;
  if (!deserializer.Deserialize(array_buffers_).ToLocal(&value)) {
    return handle_scope.Escape(v8::Null(isolate));
  }
  return handle_scope.Escape(value);
}

}  // namespace mitto
