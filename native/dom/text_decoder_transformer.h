// The transformer of a TextDecoderStream, as Blink's: it decodes each chunk
// with one of the Encoding Standard's decoders, which keeps what a chunk
// leaves of a sequence for the next.
//
// Upstream: third_party/blink/renderer/core/streams/text_decoder_transformer.h
// Upstream: third_party/blink/renderer/core/streams/text_decoder_transformer.cc

#ifndef MITTO_DOM_TEXT_DECODER_TRANSFORMER_H_
#define MITTO_DOM_TEXT_DECODER_TRANSFORMER_H_

#include <memory>
#include <string>

#include "mitto/dom/transform_stream_transformer.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-promise.h"

namespace mitto {

class TextCodec;

class TextDecoderTransformer final : public TransformStreamTransformer {
 public:
  // `encoding` is a name that CanonicalTextEncodingName() gave.
  TextDecoderTransformer(const std::string& encoding,
                         bool fatal,
                         bool ignore_bom);
  ~TextDecoderTransformer() override;

  v8::Local<v8::Promise> Transform(
      v8::Local<v8::Value> chunk,
      TransformStreamDefaultController* controller,
      bindings::ExceptionState& exception_state) override;
  v8::Local<v8::Promise> Flush(
      TransformStreamDefaultController* controller,
      bindings::ExceptionState& exception_state) override;

 private:
  // Enqueues `output`, without the byte order mark of the first output of an
  // encoding that drops it.
  v8::Local<v8::Promise> EnqueueOutput(
      std::u16string output,
      TransformStreamDefaultController* controller,
      bindings::ExceptionState& exception_state);

  std::unique_ptr<TextCodec> decoder_;
  const bool fatal_;
  const bool ignore_bom_;
  const bool encoding_has_bom_removal_;
  bool bom_seen_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_TEXT_DECODER_TRANSFORMER_H_
