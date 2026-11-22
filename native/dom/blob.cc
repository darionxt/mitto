#include "mitto/dom/blob.h"

#include <limits>
#include <utility>
#include <variant>

#include "base/check_op.h"
#include "base/containers/span.h"
#include "base/notreached.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_promise.h"
#include "mitto/bindings/v8_blob.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/dom/blob_bytes_consumer.h"
#include "mitto/dom/body_stream_buffer.h"
#include "mitto/dom/dom_exception.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/file_reader_loader.h"
#include "mitto/dom/readable_stream.h"
#include "mitto/dom/stream_chunks.h"
#include "mitto/dom/text_decoder_transformer.h"
#include "mitto/dom/transform_stream.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

// Blink's BlobFileReaderClient: reads a blob for text(), arrayBuffer() or
// bytes(), and settles the promise once the loader has all of it.
class BlobFileReaderClient final
    : public cppgc::GarbageCollected<BlobFileReaderClient>,
      public FileReaderAccumulator {
 public:
  BlobFileReaderClient(FileReadType read_type,
                       bindings::ScriptPromiseResolver* resolver,
                       ExecutionContext* context)
      : loader_(
            bindings::MakeGarbageCollected<FileReaderLoader>(this, context)),
        resolver_(resolver),
        read_type_(read_type) {}

  void Start(scoped_refptr<BlobDataHandle> handle) {
    loader_->Start(std::move(handle));
  }

  void DidFail(FileErrorCode code) override {
    FileReaderAccumulator::DidFail(code);
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    resolver_->Reject(
        isolate, bindings::ToV8(isolate, CreateFileErrorDOMException(code)));
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(loader_);
    visitor->Trace(resolver_);
    FileReaderAccumulator::Trace(visitor);
  }

 private:
  void DidFinishLoading(std::string bytes) override {
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    switch (read_type_) {
      case FileReadType::kReadAsText:
        resolver_->Resolve(
            isolate,
            bindings::ToV8String(
                isolate, FileReaderResultAsString(bytes, read_type_, u"UTF-8",
                                                  std::string_view())));
        return;
      case FileReadType::kReadAsArrayBuffer:
        resolver_->Resolve(isolate,
                           NewArrayBuffer(isolate, base::as_byte_span(bytes)));
        return;
      case FileReadType::kReadAsBytes:
        resolver_->Resolve(isolate,
                           NewUint8Array(isolate, base::as_byte_span(bytes)));
        return;
      case FileReadType::kReadAsBinaryString:
      case FileReadType::kReadAsDataURL:
        break;
    }
    NOTREACHED();
  }

  const cppgc::Member<FileReaderLoader> loader_;
  const cppgc::Member<bindings::ScriptPromiseResolver> resolver_;
  const FileReadType read_type_;
};

v8::Local<v8::Promise> ReadBlob(const scoped_refptr<BlobDataHandle>& handle,
                                FileReadType read_type) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  auto* const resolver =
      bindings::MakeGarbageCollected<bindings::ScriptPromiseResolver>(isolate);
  const v8::Local<v8::Promise> promise = resolver->Promise(isolate);
  // The loader's tasks keep the client, and the client the resolver, until
  // the read ends, as Blink's SelfKeepAlive does.
  bindings::MakeGarbageCollected<BlobFileReaderClient>(
      read_type, resolver, &ExecutionContext::Current())
      ->Start(handle);
  return promise;
}

}  // namespace

// static
Blob* Blob::Create(
    const std::optional<std::vector<
        bindings::ArrayBufferOrArrayBufferViewOrBlobOrUSVString>>& blob_parts,
    const bindings::BlobPropertyBag& options) {
  BlobData blob_data;
  blob_data.SetContentType(NormalizeType(options.type));
  if (blob_parts) {
    PopulateBlobData(
        blob_data, *blob_parts,
        /*normalize_line_endings_to_native=*/options.endings == u"native");
  }
  return Create(BlobDataHandle::Create(std::move(blob_data)));
}

// static
Blob* Blob::Create(scoped_refptr<BlobDataHandle> handle) {
  return bindings::MakeGarbageCollected<Blob>(std::move(handle));
}

// static
Blob* Blob::Create(base::span<const uint8_t> bytes,
                   std::string_view content_type) {
  BlobData blob_data;
  blob_data.SetContentType(content_type);
  blob_data.AppendBytes(bytes);
  return Create(BlobDataHandle::Create(std::move(blob_data)));
}

// static
bindings::ScriptWrappable* Blob::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<Blob>(
      info, bindings::V8Blob::kWrapperTypeInfo,
      BlobDataHandle::Create(std::string(), std::string()));
}

Blob::Blob(scoped_refptr<BlobDataHandle> handle)
    : blob_data_handle_(std::move(handle)) {}

Blob::~Blob() = default;

const bindings::WrapperTypeInfo* Blob::GetWrapperTypeInfo() const {
  return &bindings::V8Blob::kWrapperTypeInfo;
}

uint64_t Blob::Size() const {
  return blob_data_handle_->size();
}

std::u16string Blob::Type() const {
  return base::ASCIIToUTF16(blob_data_handle_->GetType());
}

Blob* Blob::Slice(int64_t start,
                  int64_t end,
                  const std::u16string& content_type,
                  bindings::ExceptionState& exception_state) const {
  ClampSliceOffsets(Size(), start, end);
  const uint64_t length = static_cast<uint64_t>(end - start);
  BlobData blob_data;
  blob_data.SetContentType(NormalizeType(content_type));
  blob_data.AppendBlob(blob_data_handle_, static_cast<uint64_t>(start), length);
  return Create(BlobDataHandle::Create(std::move(blob_data)));
}

ReadableStream* Blob::Stream() const {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  BodyStreamBuffer* const body_buffer = BodyStreamBuffer::Create(
      isolate,
      bindings::MakeGarbageCollected<BlobBytesConsumer>(
          &ExecutionContext::Current(), blob_data_handle_),
      /*signal=*/nullptr);
  return body_buffer->Stream();
}

ReadableStream* Blob::TextStream(
    bindings::ExceptionState& exception_state) const {
  ReadableStream* const body_stream = Stream();
  if (!body_stream) {
    return nullptr;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  auto* const transformer =
      bindings::MakeGarbageCollected<TextDecoderTransformer>(
          "UTF-8", /*fatal=*/false, /*ignore_bom=*/false);
  TransformStream* const transform_stream =
      TransformStream::Create(isolate, transformer, exception_state);
  if (exception_state.HadException() || !transform_stream) {
    return nullptr;
  }
  bindings::ReadableWritablePair pair;
  pair.readable = transform_stream->Readable();
  pair.writable = transform_stream->Writable();
  ReadableStream* const piped_stream =
      body_stream->PipeThrough(pair, std::nullopt, exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }
  return piped_stream;
}

v8::Local<v8::Promise> Blob::Text() {
  return ReadBlob(blob_data_handle_, FileReadType::kReadAsText);
}

v8::Local<v8::Promise> Blob::ArrayBuffer() {
  return ReadBlob(blob_data_handle_, FileReadType::kReadAsArrayBuffer);
}

v8::Local<v8::Promise> Blob::Bytes() {
  return ReadBlob(blob_data_handle_, FileReadType::kReadAsBytes);
}

void Blob::AppendTo(BlobData& blob_data) const {
  blob_data.AppendBlob(blob_data_handle_, 0, Size());
}

// static
std::string Blob::NormalizeType(std::u16string_view type) {
  if (type.size() > 65535 || !IsValidBlobType(type)) {
    return std::string();
  }
  return base::ToLowerASCII(base::UTF16ToASCII(type));
}

// static
void Blob::PopulateBlobData(
    BlobData& blob_data,
    const std::vector<bindings::ArrayBufferOrArrayBufferViewOrBlobOrUSVString>&
        parts,
    bool normalize_line_endings_to_native) {
  for (const auto& part : parts) {
    if (const auto* buffer = std::get_if<v8::Local<v8::ArrayBuffer>>(&part)) {
      // SAFETY: an ArrayBuffer holds ByteLength() bytes at Data().
      blob_data.AppendBytes(UNSAFE_BUFFERS(
          base::span(static_cast<const uint8_t*>((*buffer)->Data()),
                     (*buffer)->ByteLength())));
    } else if (const auto* view =
                   std::get_if<v8::Local<v8::ArrayBufferView>>(&part)) {
      std::string bytes((*view)->ByteLength(), '\0');
      (*view)->CopyContents(bytes.data(), bytes.size());
      blob_data.AppendBytes(base::as_byte_span(bytes));
    } else if (Blob* const* blob = std::get_if<Blob*>(&part)) {
      (*blob)->AppendTo(blob_data);
    } else {
      blob_data.AppendText(std::get<std::u16string>(part),
                           normalize_line_endings_to_native);
    }
  }
}

// static
void Blob::ClampSliceOffsets(uint64_t size, int64_t& start, int64_t& end) {
  DCHECK_NE(size, std::numeric_limits<uint64_t>::max());
  // A negative offset selects from the end.
  if (start < 0) {
    start = start + static_cast<int64_t>(size);
  }
  if (end < 0) {
    end = end + static_cast<int64_t>(size);
  }
  // Clamp the range if it exceeds the size.
  if (start < 0) {
    start = 0;
  }
  if (end < 0) {
    end = 0;
  }
  if (static_cast<uint64_t>(start) >= size) {
    start = 0;
    end = 0;
  } else if (end < start) {
    end = start;
  } else if (static_cast<uint64_t>(end) > size) {
    end = static_cast<int64_t>(size);
  }
}

}  // namespace mitto
