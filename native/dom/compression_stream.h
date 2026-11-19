// The Compression Standard's CompressionStream, as Blink's: a TransformStream of a DeflateTransformer at Blink's level 6.
//
// Upstream: third_party/blink/renderer/modules/compression/compression_stream.h
// Upstream: third_party/blink/renderer/modules/compression/compression_stream.cc

#ifndef MITTO_DOM_COMPRESSION_STREAM_H_
#define MITTO_DOM_COMPRESSION_STREAM_H_

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

class CompressionStream final : public bindings::ScriptWrappable {
 public:
  // `new CompressionStream(format)`.
  static CompressionStream* Create(const std::u16string& format,
                        bindings::ExceptionState& exception_state);

  explicit CompressionStream(TransformStream* transform);
  ~CompressionStream() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  ReadableStream* Readable() const;
  WritableStream* Writable() const;

 private:
  const cppgc::Member<TransformStream> transform_;
};

}  // namespace mitto

#endif  // MITTO_DOM_COMPRESSION_STREAM_H_
