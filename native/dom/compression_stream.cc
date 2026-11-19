#include "mitto/dom/compression_stream.h"

#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_compression_stream.h"
#include "mitto/dom/compression_format.h"
#include "mitto/dom/deflate_transformer.h"
#include "mitto/dom/transform_stream.h"

namespace mitto {

// static
CompressionStream* CompressionStream::Create(const std::u16string& format,
                        bindings::ExceptionState& exception_state) {
  const CompressionFormat format_enum =
      LookupCompressionFormat(format, exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }
  TransformStream* const transform = TransformStream::Create(
      exception_state.GetIsolate(),
      bindings::MakeGarbageCollected<DeflateTransformer>(format_enum, /*level=*/6),
      exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }
  return bindings::MakeGarbageCollected<CompressionStream>(transform);
}

CompressionStream::CompressionStream(TransformStream* transform) : transform_(transform) {}

CompressionStream::~CompressionStream() = default;

const bindings::WrapperTypeInfo* CompressionStream::GetWrapperTypeInfo() const {
  return &bindings::V8CompressionStream::kWrapperTypeInfo;
}

void CompressionStream::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(transform_);
  bindings::ScriptWrappable::Trace(visitor);
}

ReadableStream* CompressionStream::Readable() const {
  return transform_->Readable();
}

WritableStream* CompressionStream::Writable() const {
  return transform_->Writable();
}

}  // namespace mitto
