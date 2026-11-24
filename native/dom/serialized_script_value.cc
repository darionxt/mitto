#include "mitto/dom/serialized_script_value.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
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
#include "base/time/time.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/idl_member_installer.h"
#include "mitto/bindings/interfaces_without_class.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/runtime_features.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_blob.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_dom_exception.h"
#include "mitto/bindings/v8_dom_rect.h"
#include "mitto/bindings/v8_dom_rect_read_only.h"
#include "mitto/bindings/v8_file.h"
#include "mitto/bindings/v8_message_port.h"
#include "mitto/bindings/v8_quota_exceeded_error.h"
#include "mitto/bindings/v8_readable_stream.h"
#include "mitto/bindings/v8_transform_stream.h"
#include "mitto/bindings/v8_writable_stream.h"
#include "mitto/dom/blob.h"
#include "mitto/dom/blob_data.h"
#include "mitto/dom/dom_exception.h"
#include "mitto/dom/dom_rect.h"
#include "mitto/dom/dom_rect_read_only.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/file.h"
#include "mitto/dom/message_port.h"
#include "mitto/dom/quota_exceeded_error.h"
#include "mitto/dom/readable_stream.h"
#include "mitto/dom/transform_stream.h"
#include "mitto/dom/writable_stream.h"
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
constexpr uint8_t kBlobTag = 'b';
constexpr uint8_t kFileTag = 'f';
constexpr uint8_t kDOMRectTag = 'E';
constexpr uint8_t kDOMRectReadOnlyTag = 'R';
constexpr uint8_t kDOMExceptionTag = 'x';
constexpr uint8_t kQuotaExceededErrorTag = 'q';
constexpr uint8_t kReadableStreamTransferTag = 'r';
constexpr uint8_t kTransformStreamTransferTag = 'm';
constexpr uint8_t kWritableStreamTransferTag = 'w';
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
const std::array<CopiedWithoutClass, 8>& CopiedWithoutClassInterfaces() {
  static const std::array<CopiedWithoutClass, 8> interfaces = {{
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

// The interfaces whose objects Blink's V8ScriptValueSerializerForModules
// refuses to serialize for storage with a message of its own, each behind the
// feature that it checks first, if any. A Runtime has them without a class.
struct RefusedForStorage {
  // RAW_PTR_EXCLUSION: #global-scope, as WrapperTypeInfo's own.
  RAW_PTR_EXCLUSION const bindings::WrapperTypeInfo* info;
  std::optional<bindings::RuntimeFeature> feature;
  const char* message;
};
const std::array<RefusedForStorage, 11>& RefusedForStorageInterfaces() {
  using bindings::RuntimeFeature;
  static const std::array<RefusedForStorage, 11> interfaces = {{
      {&bindings::V8RTCEncodedAudioFrame::kWrapperTypeInfo, std::nullopt,
       "An RTCEncodedAudioFrame cannot be serialized for storage."},
      {&bindings::V8RTCEncodedVideoFrame::kWrapperTypeInfo, std::nullopt,
       "An RTCEncodedVideoFrame cannot be serialized for storage."},
      {&bindings::V8VideoFrame::kWrapperTypeInfo, std::nullopt,
       "A VideoFrame cannot be serialized for storage."},
      {&bindings::V8AudioData::kWrapperTypeInfo, std::nullopt,
       "AudioData cannot be serialized for storage."},
      {&bindings::V8EncodedAudioChunk::kWrapperTypeInfo, std::nullopt,
       "Encoded chunks cannot be serialized for storage."},
      {&bindings::V8EncodedVideoChunk::kWrapperTypeInfo, std::nullopt,
       "Encoded chunks cannot be serialized for storage."},
      {&bindings::V8MediaStreamTrack::kWrapperTypeInfo,
       RuntimeFeature::kMediaStreamTrackTransfer,
       "A MediaStreamTrack cannot be serialized for storage."},
      {&bindings::V8RTCDataChannel::kWrapperTypeInfo,
       RuntimeFeature::kTransferableRTCDataChannel,
       "An RTCDataChannel cannot be serialized for storage."},
      {&bindings::V8CropTarget::kWrapperTypeInfo,
       RuntimeFeature::kRegionCapture,
       "A CropTarget cannot be serialized for storage."},
      {&bindings::V8RestrictionTarget::kWrapperTypeInfo,
       RuntimeFeature::kElementCapture,
       "A RestrictionTarget cannot be serialized for storage."},
      {&bindings::V8MediaSourceHandle::kWrapperTypeInfo, std::nullopt,
       "A MediaSourceHandle cannot be serialized for storage."},
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
  Serializer(v8::Isolate* isolate,
             Transferables* transferables,
             bool for_storage,
             BlobDataHandleMap& blob_data_handles)
      : isolate_(isolate),
        serializer_(isolate, this),
        transferables_(transferables),
        for_storage_(for_storage),
        blob_data_handles_(blob_data_handles) {}
  ~Serializer() override = default;

  // V8ScriptValueSerializer::Serialize(): the bytes, the transferred
  // ArrayBuffers' contents, the SharedArrayBuffers' and the WebAssembly
  // modules, or false after throwing.
  bool Serialize(
      v8::Local<v8::Value> value,
      std::vector<uint8_t>& data,
      std::vector<std::shared_ptr<v8::BackingStore>>& array_buffers,
      std::vector<std::shared_ptr<v8::BackingStore>>& shared_array_buffers,
      std::vector<v8::CompiledWasmModule>& wasm_modules,
      std::vector<MessagePortChannel>& stream_channels) {
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
      // TransferReadableStreams(), TransferWritableStreams() and
      // TransferTransformStreams(), in this order, which the indexes that
      // WriteDOMObject() wrote count on.
      if (!TransferStreams(stream_channels)) {
        return false;
      }
    }
    // The SharedArrayBuffers that the value holds go only from a context
    // that may post them, which
    // CheckSharedArrayBufferTransferAllowedAndReport() checks once the rest is
    // done, and then share their memory with the copy:
    // CloneSharedArrayBuffers().
    if (!shared_array_buffers_.empty()) {
      const ExecutionContext* const execution_context =
          ExecutionContext::From(isolate_, context);
      if (!execution_context ||
          !execution_context->SharedArrayBufferTransferAllowed()) {
        mitto::ThrowDataCloneError(
            isolate_,
            "SharedArrayBuffer transfer requires self.crossOriginIsolated.");
        return false;
      }
      for (const v8::Global<v8::SharedArrayBuffer>& buffer :
           shared_array_buffers_) {
        shared_array_buffers.push_back(buffer.Get(isolate_)->GetBackingStore());
      }
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
    // Objects that Blink refuses whatever is behind them here: a Known Gap
    // gives an EventTarget's, as an RTCDataChannel's or an OffscreenCanvas's,
    // the class of EventTarget. An OffscreenCanvas in the transfer list was
    // refused already.
    if (for_storage_ && ThrowIfRefusedForStorage(object)) {
      return v8::Nothing<bool>();
    }
    if (bindings::PerIsolateData::From(isolate_)->HasInstance(
            isolate_, bindings::V8OffscreenCanvas::kWrapperTypeInfo, object)) {
      mitto::ThrowDataCloneError(isolate_,
                                 "An OffscreenCanvas could not be cloned "
                                 "because it was not transferred.");
      return v8::Nothing<bool>();
    }
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
    if (for_storage_) {
      mitto::ThrowDataCloneError(
          isolate, "A SharedArrayBuffer can not be serialized for storage.");
      return v8::Nothing<uint32_t>();
    }
    // Its index among the value's SharedArrayBuffers, which the copy reads
    // its memory by, the same for each place that holds it. The transfer is
    // checked once the value is written, as Blink checks it then.
    const auto found = std::ranges::find_if(
        shared_array_buffers_,
        [&shared_array_buffer](
            const v8::Global<v8::SharedArrayBuffer>& buffer) {
          return buffer == shared_array_buffer;
        });
    if (found != shared_array_buffers_.end()) {
      return v8::Just(
          base::checked_cast<uint32_t>(found - shared_array_buffers_.begin()));
    }
    shared_array_buffers_.emplace_back(isolate, shared_array_buffer);
    return v8::Just(
        base::checked_cast<uint32_t>(shared_array_buffers_.size() - 1));
  }

  v8::Maybe<uint32_t> GetWasmModuleTransferId(
      v8::Isolate* isolate,
      v8::Local<v8::WasmModuleObject> module) override {
    if (for_storage_) {
      mitto::ThrowDataCloneError(
          isolate, "A WebAssembly.Module can not be serialized for storage.");
      return v8::Nothing<uint32_t>();
    }
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

  // The value carries `handle`, which its copies read their bytes from, and
  // names it by its UUID, as Blink's AttachBlob() does.
  std::u16string AttachBlob(const scoped_refptr<BlobDataHandle>& handle) {
    blob_data_handles_->emplace(handle->Uuid(), handle);
    return base::ASCIIToUTF16(handle->Uuid());
  }

  // V8ScriptValueSerializer::WriteFile() without a blob info array.
  void WriteFile(const File& file) {
    // The path of a file that has one on disk, which a page's never has.
    WriteUTF8String(std::u16string());
    WriteUTF8String(file.Name());
    WriteUTF8String(file.WebkitRelativePath());
    WriteUTF8String(AttachBlob(file.GetBlobDataHandle()));
    WriteUTF8String(file.Type());
    // Blink always writes the metadata now, after a 1 that once said so.
    serializer_.WriteUint32(1);
    serializer_.WriteUint64(file.Size());
    const std::optional<base::Time> last_modified =
        file.LastModifiedTimeForSerialization();
    serializer_.WriteDouble(
        last_modified
            ? last_modified->InMillisecondsFSinceUnixEpochIgnoringNull()
            : std::numeric_limits<double>::quiet_NaN());
    // Whether the user sees it: a file that a page made is not one of theirs.
    serializer_.WriteUint32(0);
  }

  // V8ScriptValueSerializer::WriteDOMObject(), for the objects of the
  // Runtime's: whether it wrote `wrappable`. It throws why not where Blink
  // does, and sets `thrown_`.
  bool WriteDOMObject(bindings::ScriptWrappable& wrappable) {
    const bindings::WrapperTypeInfo* const info =
        wrappable.GetWrapperTypeInfo();
    // Blink's TypeDispatcher::ToMostDerived(): the object's own interface.
    if (info == &bindings::V8Blob::kWrapperTypeInfo) {
      const auto& blob = static_cast<const Blob&>(wrappable);
      WriteAndRequireInterfaceTag(kBlobTag);
      WriteUTF8String(AttachBlob(blob.GetBlobDataHandle()));
      WriteUTF8String(blob.Type());
      serializer_.WriteUint64(blob.Size());
      return true;
    }
    if (info == &bindings::V8File::kWrapperTypeInfo) {
      WriteAndRequireInterfaceTag(kFileTag);
      WriteFile(static_cast<const File&>(wrappable));
      return true;
    }
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
    if (info == &bindings::V8ReadableStream::kWrapperTypeInfo) {
      return WriteReadableStream(static_cast<ReadableStream&>(wrappable));
    }
    if (info == &bindings::V8WritableStream::kWrapperTypeInfo) {
      return WriteWritableStream(static_cast<WritableStream&>(wrappable));
    }
    if (info == &bindings::V8TransformStream::kWrapperTypeInfo) {
      return WriteTransformStream(static_cast<TransformStream&>(wrappable));
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

  // The index of `object` in `list`, or `list`'s size if it is not there.
  template <typename T>
  static size_t IndexIn(const std::vector<cppgc::Persistent<T>>& list,
                        const T* object) {
    size_t index = 0;
    while (index < list.size() && list[index].Get() != object) {
      ++index;
    }
    return index;
  }

  // Throws a DataCloneError with `message` where Blink does, and remembers
  // that it threw.
  bool RefuseDOMObject(std::string_view message) {
    mitto::ThrowDataCloneError(isolate_, message);
    thrown_ = true;
    return false;
  }

  // WriteReadableStream().
  bool WriteReadableStream(ReadableStream& stream) {
    const size_t index =
        transferables_ ? IndexIn(transferables_->readable_streams, &stream) : 0;
    if (!transferables_ || index == transferables_->readable_streams.size()) {
      return RefuseDOMObject(
          "A ReadableStream could not be cloned because it was not "
          "transferred.");
    }
    if (stream.IsLocked()) {
      return RefuseDOMObject(
          "A ReadableStream could not be cloned because it was locked");
    }
    WriteAndRequireInterfaceTag(kReadableStreamTransferTag);
    serializer_.WriteUint32(base::checked_cast<uint32_t>(index));
    return true;
  }

  // WriteWritableStream().
  bool WriteWritableStream(WritableStream& stream) {
    const size_t index =
        transferables_ ? IndexIn(transferables_->writable_streams, &stream) : 0;
    if (!transferables_ || index == transferables_->writable_streams.size()) {
      return RefuseDOMObject(
          "A WritableStream could not be cloned because it was not "
          "transferred.");
    }
    if (stream.Locked()) {
      return RefuseDOMObject(
          "A WritableStream could not be cloned because it was locked");
    }
    WriteAndRequireInterfaceTag(kWritableStreamTransferTag);
    // The index counts on TransferReadableStreams() coming first.
    serializer_.WriteUint32(base::checked_cast<uint32_t>(
        index + transferables_->readable_streams.size()));
    return true;
  }

  // What WriteDOMObject() writes of a TransformStream.
  bool WriteTransformStream(TransformStream& stream) {
    const size_t index =
        transferables_ ? IndexIn(transferables_->transform_streams, &stream)
                       : 0;
    if (!transferables_ || index == transferables_->transform_streams.size()) {
      return RefuseDOMObject(
          "A TransformStream could not be cloned because it was not "
          "transferred.");
    }
    // https://streams.spec.whatwg.org/#ts-transfer
    // 3. If ! IsReadableStreamLocked(readable) is true, throw a
    //    "DataCloneError" DOMException.
    // 4. If ! IsWritableStreamLocked(writable) is true, throw a
    //    "DataCloneError" DOMException.
    if (stream.Readable()->Locked() || stream.Writable()->Locked()) {
      return RefuseDOMObject(
          "A TransformStream could not be cloned because it was locked");
    }
    WriteAndRequireInterfaceTag(kTransformStreamTransferTag);
    // TransformStreams use two ports each. The stored index is the index of
    // the first one. The first TransformStream is stored in the array after
    // all the ReadableStreams and WritableStreams.
    serializer_.WriteUint32(base::checked_cast<uint32_t>(
        index * 2 + transferables_->readable_streams.size() +
        transferables_->writable_streams.size()));
    return true;
  }

  // AddStreamChannel(): a new port of the context's, a window's or a
  // worker's, entangled with the end that `stream_channels` gets, which the
  // value carries.
  MessagePort* AddStreamChannel(
      ExecutionContext* context,
      std::vector<MessagePortChannel>& stream_channels) {
    // 2. Let port1 be a new MessagePort in the current Realm.
    // 3. Let port2 be a new MessagePort in the current Realm.
    auto [local, remote] = MessagePortChannel::CreatePair();
    auto* const local_port =
        bindings::MakeGarbageCollected<MessagePort>(context);
    // 4. Entangle port1 and port2.
    local_port->Entangle(std::move(local), nullptr);
    // 9. Set dataHolder.[[port]] to ! StructuredSerializeWithTransfer(port2,
    //    « port2 »).
    stream_channels.push_back(std::move(remote));
    return local_port;
  }

  // The streams of the transfer list, each serialized over a new port, as
  // SerializedScriptValue::TransferReadableStream() and its kin do: false
  // after throwing.
  bool TransferStreams(std::vector<MessagePortChannel>& stream_channels) {
    ExecutionContext* const context =
        ExecutionContext::From(isolate_, isolate_->GetCurrentContext());
    // What a stream's Serialize() throws stays on the isolate for the caller.
    bindings::ExceptionState exception_state(isolate_);
    for (const cppgc::Persistent<ReadableStream>& stream :
         transferables_->readable_streams) {
      stream->Serialize(isolate_, AddStreamChannel(context, stream_channels),
                        exception_state);
      if (exception_state.HadException()) {
        return false;
      }
    }
    for (const cppgc::Persistent<WritableStream>& stream :
         transferables_->writable_streams) {
      stream->Serialize(isolate_, AddStreamChannel(context, stream_channels),
                        exception_state);
      if (exception_state.HadException()) {
        return false;
      }
    }
    for (const cppgc::Persistent<TransformStream>& stream :
         transferables_->transform_streams) {
      stream->Readable()->Serialize(isolate_,
                                    AddStreamChannel(context, stream_channels),
                                    exception_state);
      if (exception_state.HadException()) {
        return false;
      }
      stream->Writable()->Serialize(isolate_,
                                    AddStreamChannel(context, stream_channels),
                                    exception_state);
      if (exception_state.HadException()) {
        return false;
      }
    }
    return true;
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

  // Whether `object` is of an interface that Blink refuses to serialize for
  // storage with a message of its own, which it then threw.
  bool ThrowIfRefusedForStorage(v8::Local<v8::Object> object) {
    const bindings::PerIsolateData* const data =
        bindings::PerIsolateData::From(isolate_);
    for (const RefusedForStorage& refused : RefusedForStorageInterfaces()) {
      if ((!refused.feature || data->features().IsEnabled(*refused.feature)) &&
          data->HasInstance(isolate_, *refused.info, object)) {
        mitto::ThrowDataCloneError(isolate_, refused.message);
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
  // SerializeOptions::kForStorage, as IndexedDB serializes: what cannot be
  // stored throws where the serializer meets it.
  const bool for_storage_;
  const raw_ref<BlobDataHandleMap> blob_data_handles_;
  std::vector<uint8_t> required_interfaces_;
  std::vector<v8::CompiledWasmModule> wasm_modules_;
  // The value's SharedArrayBuffers, each once, in the order the serializer
  // met them. V8 hands each over in a handle scope of its own, which ends
  // before the value is written.
  std::vector<v8::Global<v8::SharedArrayBuffer>> shared_array_buffers_;
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
    case kBlobTag:
      return &bindings::V8Blob::kWrapperTypeInfo;
    case kFileTag:
      return &bindings::V8File::kWrapperTypeInfo;
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
    case kReadableStreamTransferTag:
      return &bindings::V8ReadableStream::kWrapperTypeInfo;
    case kWritableStreamTransferTag:
      return &bindings::V8WritableStream::kWrapperTypeInfo;
    case kTransformStreamTransferTag:
      return &bindings::V8TransformStream::kWrapperTypeInfo;
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
               const std::vector<std::shared_ptr<v8::BackingStore>>&
                   shared_array_buffers,
               const std::vector<v8::CompiledWasmModule>& wasm_modules,
               std::vector<MessagePortChannel>* stream_channels,
               const BlobDataHandleMap& blob_data_handles)
      : isolate_(isolate),
        deserializer_(isolate, data.data(), data.size(), this),
        data_(data),
        ports_(ports),
        shared_array_buffers_(shared_array_buffers),
        wasm_modules_(wasm_modules),
        stream_channels_(stream_channels),
        blob_data_handles_(blob_data_handles) {}
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

  // A SharedArrayBuffer on the memory of the original's, as Blink's
  // DOMSharedArrayBuffer::Create() makes it. One that stayed in its agent
  // cluster, as one that a value from another process held did, makes the
  // value null, where Blink throws "Unable to deserialize
  // SharedArrayBuffer.", which no page sees either.
  v8::MaybeLocal<v8::SharedArrayBuffer> GetSharedArrayBufferFromId(
      v8::Isolate* isolate,
      uint32_t id) override {
    if (id < shared_array_buffers_->size()) {
      return v8::SharedArrayBuffer::New(isolate, (*shared_array_buffers_)[id]);
    }
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

  // CreateEntangledPort(): a new port of the context's that holds the end of
  // the port of the stream at `index`, or null if there is none.
  MessagePort* TakeStreamPort(uint32_t index) {
    if (!stream_channels_ || index >= stream_channels_->size() ||
        !(*stream_channels_)[index].is_valid()) {
      return nullptr;
    }
    auto* const port = bindings::MakeGarbageCollected<MessagePort>(
        ExecutionContext::From(isolate_, isolate_->GetCurrentContext()));
    port->Entangle(std::move((*stream_channels_)[index]), nullptr);
    return port;
  }

  // The blob that the value carries by `uuid`. One that it does not carry,
  // as a value that came from another Page does not, reads as empty, of
  // `type`, where Chrome's browser would give the blob's bytes: a Known Gap.
  scoped_refptr<BlobDataHandle> BlobDataHandleOf(const std::u16string& uuid,
                                                 const std::u16string& type) {
    const auto found = blob_data_handles_->find(base::UTF16ToUTF8(uuid));
    if (found != blob_data_handles_->end()) {
      return found->second;
    }
    return BlobDataHandle::Create(std::string(), Blob::NormalizeType(type));
  }

  // V8ScriptValueDeserializer::ReadFile(), of the version the Runtime writes.
  v8::MaybeLocal<v8::Object> ReadFile() {
    std::u16string path;
    std::u16string name;
    std::u16string relative_path;
    std::u16string uuid;
    std::u16string type;
    uint32_t has_snapshot = 0;
    uint64_t size = 0;
    std::optional<base::Time> last_modified;
    if (!ReadUTF8String(path) || !ReadUTF8String(name) ||
        !ReadUTF8String(relative_path) || !ReadUTF8String(uuid) ||
        !ReadUTF8String(type) || !deserializer_.ReadUint32(&has_snapshot)) {
      return {};
    }
    if (has_snapshot) {
      double last_modified_ms = 0;
      if (!deserializer_.ReadUint64(&size) ||
          !deserializer_.ReadDouble(&last_modified_ms)) {
        return {};
      }
      if (std::isfinite(last_modified_ms)) {
        last_modified =
            base::Time::FromMillisecondsSinceUnixEpoch(last_modified_ms);
      }
    }
    uint32_t is_user_visible = 0;
    if (!deserializer_.ReadUint32(&is_user_visible)) {
      return {};
    }
    return NewWrapper(File::Create(std::move(name), std::move(relative_path),
                                   last_modified,
                                   BlobDataHandleOf(uuid, type)));
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
      case kBlobTag: {
        std::u16string uuid;
        std::u16string type;
        uint64_t size = 0;
        if (!ReadUTF8String(uuid) || !ReadUTF8String(type) ||
            !deserializer_.ReadUint64(&size)) {
          return {};
        }
        return NewWrapper(Blob::Create(BlobDataHandleOf(uuid, type)));
      }
      case kFileTag:
        return ReadFile();
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
      case kReadableStreamTransferTag: {
        uint32_t index = 0;
        MessagePort* port = nullptr;
        if (!deserializer_.ReadUint32(&index) ||
            !(port = TakeStreamPort(index))) {
          return {};
        }
        bindings::ExceptionState exception_state(isolate_);
        ReadableStream* const readable =
            ReadableStream::Deserialize(isolate_, port, exception_state);
        if (!readable) {
          return {};
        }
        return NewWrapper(readable);
      }
      case kWritableStreamTransferTag: {
        uint32_t index = 0;
        MessagePort* port = nullptr;
        if (!deserializer_.ReadUint32(&index) ||
            !(port = TakeStreamPort(index))) {
          return {};
        }
        bindings::ExceptionState exception_state(isolate_);
        WritableStream* const writable =
            WritableStream::Deserialize(isolate_, port, exception_state);
        if (!writable) {
          return {};
        }
        return NewWrapper(writable);
      }
      case kTransformStreamTransferTag: {
        uint32_t index = 0;
        if (!deserializer_.ReadUint32(&index) ||
            index == std::numeric_limits<uint32_t>::max()) {
          return {};
        }
        MessagePort* const port_for_readable = TakeStreamPort(index);
        MessagePort* const port_for_writable = TakeStreamPort(index + 1);
        if (!port_for_readable || !port_for_writable) {
          return {};
        }
        // https://streams.spec.whatwg.org/#ts-transfer
        // 1. Let readableRecord be !
        //    StructuredDeserializeWithTransfer(dataHolder.[[readable]], the
        //    current Realm).
        bindings::ExceptionState exception_state(isolate_);
        ReadableStream* const readable = ReadableStream::Deserialize(
            isolate_, port_for_readable, exception_state);
        if (!readable) {
          return {};
        }
        // 2. Let writableRecord be !
        //    StructuredDeserializeWithTransfer(dataHolder.[[writable]], the
        //    current Realm).
        WritableStream* const writable = WritableStream::Deserialize(
            isolate_, port_for_writable, exception_state);
        if (!writable) {
          return {};
        }
        // 3. Set value.[[readable]] to readableRecord.[[Deserialized]].
        // 4. Set value.[[writable]] to writableRecord.[[Deserialized]].
        // 5. Set value.[[backpressure]], value.[[backpressureChangePromise]],
        //    and value.[[controller]] to undefined.
        return NewWrapper(bindings::MakeGarbageCollected<TransformStream>(
            readable, writable));
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
  const raw_ref<const std::vector<std::shared_ptr<v8::BackingStore>>>
      shared_array_buffers_;
  const raw_ref<const std::vector<v8::CompiledWasmModule>> wasm_modules_;
  // The other ends of the ports of the streams that the value transferred,
  // which reading takes, or null for none.
  const raw_ptr<std::vector<MessagePortChannel>> stream_channels_;
  const raw_ref<const BlobDataHandleMap> blob_data_handles_;
  // The interfaces whose interface objects the context made before V8
  // started to read.
  std::vector<const bindings::WrapperTypeInfo*> prepared_;
};

}  // namespace

Transferables::Transferables() = default;
Transferables::~Transferables() = default;

struct SerializedScriptValue::StreamChannels {
  std::vector<MessagePortChannel> channels;
};

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
    if (ReadableStream* const stream =
            bindings::V8ReadableStream::ToWrappable(isolate, object)) {
      for (const cppgc::Persistent<ReadableStream>& earlier :
           transferables.readable_streams) {
        if (earlier.Get() == stream) {
          ThrowDataCloneError(
              isolate,
              base::StrCat({"ReadableStream at index ", IndexText(i),
                            " is a duplicate of an earlier ReadableStream."}));
          return false;
        }
      }
      transferables.readable_streams.emplace_back(stream);
      continue;
    }
    if (WritableStream* const stream =
            bindings::V8WritableStream::ToWrappable(isolate, object)) {
      for (const cppgc::Persistent<WritableStream>& earlier :
           transferables.writable_streams) {
        if (earlier.Get() == stream) {
          ThrowDataCloneError(
              isolate,
              base::StrCat({"WritableStream at index ", IndexText(i),
                            " is a duplicate of an earlier WritableStream."}));
          return false;
        }
      }
      transferables.writable_streams.emplace_back(stream);
      continue;
    }
    if (TransformStream* const stream =
            bindings::V8TransformStream::ToWrappable(isolate, object)) {
      for (const cppgc::Persistent<TransformStream>& earlier :
           transferables.transform_streams) {
        if (earlier.Get() == stream) {
          ThrowDataCloneError(
              isolate,
              base::StrCat({"TransformStream at index ", IndexText(i),
                            " is a duplicate of an earlier TransformStream."}));
          return false;
        }
      }
      transferables.transform_streams.emplace_back(stream);
      continue;
    }
    // ImageBitmap and OffscreenCanvas, which Blink transfers, have no class
    // in the Runtime, and so no object of theirs moves: a Known Gap.
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
  return SerializeWith(isolate, value, transferables, /*for_storage=*/false);
}

// static
std::optional<SerializedScriptValue> SerializedScriptValue::SerializeForStorage(
    v8::Isolate* isolate,
    v8::Local<v8::Value> value) {
  return SerializeWith(isolate, value, nullptr, /*for_storage=*/true);
}

// static
std::optional<SerializedScriptValue> SerializedScriptValue::SerializeWith(
    v8::Isolate* isolate,
    v8::Local<v8::Value> value,
    Transferables* transferables,
    bool for_storage) {
  SerializedScriptValue serialized;
  Serializer serializer(isolate, transferables, for_storage,
                        serialized.blob_data_handles_);
  std::vector<v8::CompiledWasmModule> wasm_modules;
  std::vector<MessagePortChannel> stream_channels;
  if (!serializer.Serialize(value, serialized.data_, serialized.array_buffers_,
                            serialized.shared_array_buffers_, wasm_modules,
                            stream_channels)) {
    return std::nullopt;
  }
  if (!stream_channels.empty()) {
    serialized.streams_ = std::make_shared<StreamChannels>();
    serialized.streams_->channels = std::move(stream_channels);
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
    std::vector<uint8_t> data,
    BlobDataHandleMap blob_data_handles) {
  SerializedScriptValue serialized;
  serialized.data_ = std::move(data);
  serialized.blob_data_handles_ = std::move(blob_data_handles);
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
  Deserializer deserializer(isolate, data_, ports, shared_array_buffers_,
                            wasm_modules_ ? *wasm_modules_ : *kNone,
                            streams_ ? &streams_->channels : nullptr,
                            blob_data_handles_);
  v8::Local<v8::Value> value;
  if (!deserializer.Deserialize(array_buffers_).ToLocal(&value)) {
    return handle_scope.Escape(v8::Null(isolate));
  }
  return handle_scope.Escape(value);
}

}  // namespace mitto
