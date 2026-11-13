// x-user-defined, as Blink's TextCodecUserDefined decodes and encodes it:
// ASCII as it is, and each byte from 0x80 as U+F780 plus the byte's value less
// 0x80.
//
// Upstream: third_party/blink/renderer/platform/wtf/text/text_codec_user_defined.h
// Upstream: third_party/blink/renderer/platform/wtf/text/text_codec_user_defined.cc

#ifndef MITTO_ENCODING_TEXT_CODEC_USER_DEFINED_H_
#define MITTO_ENCODING_TEXT_CODEC_USER_DEFINED_H_

#include <stdint.h>

#include <string>
#include <string_view>

#include "base/containers/span.h"
#include "mitto/encoding/text_codec.h"

namespace mitto {

class TextCodecUserDefined final : public TextCodec {
 public:
  static void RegisterEncodingNames(EncodingNameRegistrar registrar);
  static void RegisterCodecs(TextCodecRegistrar registrar);

  std::u16string Decode(base::span<const uint8_t> data,
                        FlushBehavior flush,
                        bool stop_on_error,
                        bool& saw_error) override;
  std::string Encode(std::u16string_view text,
                     UnencodableHandling handling) override;
};

}  // namespace mitto

#endif  // MITTO_ENCODING_TEXT_CODEC_USER_DEFINED_H_
