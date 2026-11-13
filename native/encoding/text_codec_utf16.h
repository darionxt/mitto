// UTF-16LE and UTF-16BE, as Blink's TextCodecUtf16 decodes them: a lone
// surrogate gives U+FFFD, and a byte or a lead surrogate that a call leaves
// waits for the next, or gives U+FFFD at the end of the data.
//
// Upstream: third_party/blink/renderer/platform/wtf/text/text_codec_utf16.h
// Upstream: third_party/blink/renderer/platform/wtf/text/text_codec_utf16.cc

#ifndef MITTO_ENCODING_TEXT_CODEC_UTF16_H_
#define MITTO_ENCODING_TEXT_CODEC_UTF16_H_

#include <stdint.h>

#include <string>
#include <string_view>

#include "base/containers/span.h"
#include "mitto/encoding/text_codec.h"

namespace mitto {

class TextCodecUtf16 final : public TextCodec {
 public:
  static bool IsSupported(std::string_view canonical_name);
  static void RegisterEncodingNames(EncodingNameRegistrar registrar);
  static void RegisterCodecs(TextCodecRegistrar registrar);

  explicit TextCodecUtf16(bool little_endian);
  ~TextCodecUtf16() override;

  std::u16string Decode(base::span<const uint8_t> data,
                        FlushBehavior flush,
                        bool stop_on_error,
                        bool& saw_error) override;

 private:
  const bool little_endian_;
  bool have_lead_byte_ = false;
  bool have_lead_surrogate_ = false;
  uint8_t lead_byte_ = 0;
  char16_t lead_surrogate_ = 0;
};

}  // namespace mitto

#endif  // MITTO_ENCODING_TEXT_CODEC_UTF16_H_
