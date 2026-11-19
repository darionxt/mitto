#include "mitto/dom/decompression_stream.h"

#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_decompression_stream.h"
#include "mitto/dom/compression_format.h"
#include "mitto/dom/inflate_transformer.h"
#include "mitto/dom/transform_stream.h"

namespace mitto {

// static
DecompressionStream* DecompressionStream::Create(const std::u16string& format,
                        bindings::ExceptionState& exception_state) {
  const CompressionFormat format_enum =
      LookupCompressionFormat(format, exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }
  TransformStream* const transform = TransformStream::Create(
      exception_state.GetIsolate(),
      bindings::MakeGarbageCollected<InflateTransformer>(format_enum),
      exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }
  return bindings::MakeGarbageCollected<DecompressionStream>(transform);
}

DecompressionStream::DecompressionStream(TransformStream* transform) : transform_(transform) {}

DecompressionStream::~DecompressionStream() = default;

const bindings::WrapperTypeInfo* DecompressionStream::GetWrapperTypeInfo() const {
  return &bindings::V8DecompressionStream::kWrapperTypeInfo;
}

void DecompressionStream::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(transform_);
  bindings::ScriptWrappable::Trace(visitor);
}

ReadableStream* DecompressionStream::Readable() const {
  return transform_->Readable();
}

WritableStream* DecompressionStream::Writable() const {
  return transform_->Writable();
}

}  // namespace mitto
