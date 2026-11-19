// The Encoding Standard's TextEncoderStream, as Blink's: a TransformStream
// that encodes each string chunk as UTF-8, and keeps a high surrogate at the
// end of a chunk for the next.
//
// Upstream: third_party/blink/renderer/modules/encoding/text_encoder_stream.h
// Upstream: third_party/blink/renderer/modules/encoding/text_encoder_stream.cc

#ifndef MITTO_DOM_TEXT_ENCODER_STREAM_H_
#define MITTO_DOM_TEXT_ENCODER_STREAM_H_

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

class TextEncoderStream final : public bindings::ScriptWrappable {
 public:
  // `new TextEncoderStream()`.
  static TextEncoderStream* Create(bindings::ExceptionState& exception_state);

  explicit TextEncoderStream(TransformStream* transform);
  ~TextEncoderStream() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  std::u16string Encoding() const;
  ReadableStream* Readable() const;
  WritableStream* Writable() const;

 private:
  class Transformer;

  const cppgc::Member<TransformStream> transform_;
};

}  // namespace mitto

#endif  // MITTO_DOM_TEXT_ENCODER_STREAM_H_
