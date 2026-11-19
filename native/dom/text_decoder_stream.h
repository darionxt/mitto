// The Encoding Standard's TextDecoderStream, as Blink's: a TransformStream of
// a TextDecoderTransformer.
//
// Upstream: third_party/blink/renderer/modules/encoding/text_decoder_stream.h
// Upstream: third_party/blink/renderer/modules/encoding/text_decoder_stream.cc

#ifndef MITTO_DOM_TEXT_DECODER_STREAM_H_
#define MITTO_DOM_TEXT_DECODER_STREAM_H_

#include <string>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

namespace bindings {
class ExceptionState;
struct TextDecoderOptions;
}  // namespace bindings

class ReadableStream;
class TransformStream;
class WritableStream;

class TextDecoderStream final : public bindings::ScriptWrappable {
 public:
  // `new TextDecoderStream(label, options)`.
  static TextDecoderStream* Create(const std::u16string& label,
                                   const bindings::TextDecoderOptions& options,
                                   bindings::ExceptionState& exception_state);

  TextDecoderStream(TransformStream* transform,
                    std::string encoding,
                    bool fatal,
                    bool ignore_bom);
  ~TextDecoderStream() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  std::u16string Encoding() const;
  bool Fatal() const { return fatal_; }
  bool IgnoreBOM() const { return ignore_bom_; }
  ReadableStream* Readable() const;
  WritableStream* Writable() const;

 private:
  const cppgc::Member<TransformStream> transform_;
  const std::string encoding_;
  const bool fatal_;
  const bool ignore_bom_;
};

}  // namespace mitto

#endif  // MITTO_DOM_TEXT_DECODER_STREAM_H_
