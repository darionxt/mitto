// The Encoding Standard's TextEncoder, as Blink's: UTF-8 alone, with U+FFFD
// for a lone surrogate, which the bindings' conversion to a USVString gives.
//
// Upstream: third_party/blink/renderer/modules/encoding/text_encoder.h
// Upstream: third_party/blink/renderer/modules/encoding/text_encoder.cc

#ifndef MITTO_DOM_TEXT_ENCODER_H_
#define MITTO_DOM_TEXT_ENCODER_H_

#include <string>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-typed-array.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class TextEncoder final : public bindings::ScriptWrappable {
 public:
  static TextEncoder* Create(bindings::ExceptionState& exception_state);

  TextEncoder();
  ~TextEncoder() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  std::u16string Encoding() const;
  v8::Local<v8::Uint8Array> Encode(const std::u16string& input,
                                   bindings::ExceptionState& exception_state);
  // A TextEncoderEncodeIntoResult: how many code units of `source` it read,
  // and how many bytes it wrote into `destination`.
  v8::Local<v8::Object> EncodeInto(const std::u16string& source,
                                   v8::Local<v8::Uint8Array> destination);
};

}  // namespace mitto

#endif  // MITTO_DOM_TEXT_ENCODER_H_
