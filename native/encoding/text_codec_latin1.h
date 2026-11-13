// windows-1252, which the Encoding Standard's labels for ISO-8859-1 and ASCII
// name too, as Blink's TextCodecLatin1 decodes it: one character for each
// byte, from windows-1252's table.
//
// Upstream: third_party/blink/renderer/platform/wtf/text/text_codec_latin1.h
// Upstream: third_party/blink/renderer/platform/wtf/text/text_codec_latin1.cc

#ifndef MITTO_ENCODING_TEXT_CODEC_LATIN1_H_
#define MITTO_ENCODING_TEXT_CODEC_LATIN1_H_

#include <stdint.h>

#include <string>

#include "base/containers/span.h"
#include "mitto/encoding/text_codec.h"

namespace mitto {

class TextCodecLatin1 final : public TextCodec {
 public:
  static void RegisterEncodingNames(EncodingNameRegistrar registrar);
  static void RegisterCodecs(TextCodecRegistrar registrar);

  std::u16string Decode(base::span<const uint8_t> data,
                        FlushBehavior flush,
                        bool stop_on_error,
                        bool& saw_error) override;
};

}  // namespace mitto

#endif  // MITTO_ENCODING_TEXT_CODEC_LATIN1_H_
