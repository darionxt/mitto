// The Compression Standard's DecompressionStream, as Blink's: a TransformStream of an InflateTransformer.
//
// Upstream: third_party/blink/renderer/modules/compression/decompression_stream.h
// Upstream: third_party/blink/renderer/modules/compression/decompression_stream.cc

#ifndef MITTO_DOM_DECOMPRESSION_STREAM_H_
#define MITTO_DOM_DECOMPRESSION_STREAM_H_

#include <string>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class ReadableStream;
class TransformStream;
class WritableStream;

class DecompressionStream final : public bindings::ScriptWrappable {
 public:
  // `new DecompressionStream(format)`.
  static DecompressionStream* Create(const std::u16string& format,
                        bindings::ExceptionState& exception_state);

  explicit DecompressionStream(TransformStream* transform);
  ~DecompressionStream() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  ReadableStream* Readable() const;
  WritableStream* Writable() const;

 private:
  const cppgc::Member<TransformStream> transform_;
};

}  // namespace mitto

#endif  // MITTO_DOM_DECOMPRESSION_STREAM_H_
