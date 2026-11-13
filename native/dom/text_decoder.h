// The Encoding Standard's TextDecoder, as Blink's: any encoding that the
// registry has a label for but replacement, with Chrome's errors, a byte order
// mark of UTF-8 or UTF-16 dropped unless `ignoreBOM`, and `stream`, which
// keeps a sequence that the input cuts off for the next call.
//
// Upstream: third_party/blink/renderer/modules/encoding/text_decoder.h
// Upstream: third_party/blink/renderer/modules/encoding/text_decoder.cc
// Upstream: third_party/blink/renderer/modules/encoding/encoding.cc

#ifndef MITTO_DOM_TEXT_DECODER_H_
#define MITTO_DOM_TEXT_DECODER_H_

#include <stdint.h>

#include <memory>
#include <optional>
#include <string>

#include "base/containers/span.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/encoding/text_codec.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class TextDecoder final : public bindings::ScriptWrappable {
 public:
  static TextDecoder* Create(const std::u16string& label,
                             const bindings::TextDecoderOptions& options,
                             bindings::ExceptionState& exception_state);

  // `encoding` is the canonical name of the encoding.
  TextDecoder(std::string encoding, bool fatal, bool ignore_bom);
  ~TextDecoder() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  std::u16string Encoding() const;
  bool Fatal() const { return fatal_; }
  bool IgnoreBOM() const { return ignore_bom_; }
  std::u16string Decode(std::optional<base::span<const uint8_t>> input,
                        const bindings::TextDecodeOptions& options,
                        bindings::ExceptionState& exception_state);

 private:
  const std::string encoding_;
  const bool fatal_;
  const bool ignore_bom_;
  // Whether the stream's first characters have gone by, so that a byte order
  // mark is no longer one.
  bool bom_seen_ = false;
  // Whether the last call had `stream`, so that this one goes on with its
  // stream.
  bool do_not_flush_ = false;
  // The decoder of the stream, kept after a stream ends for the next.
  std::unique_ptr<TextCodec> codec_;
};

}  // namespace mitto

#endif  // MITTO_DOM_TEXT_DECODER_H_
