// The Encoding Standard's replacement encoding, as Blink's
// TextCodecReplacement decodes it: one U+FFFD for the whole stream, if it has
// any bytes. Its labels, such as iso-2022-kr, name encodings that a page must
// not be decoded with. TextDecoder refuses it, as Blink's does.
//
// Upstream: third_party/blink/renderer/platform/wtf/text/text_codec_replacement.h
// Upstream: third_party/blink/renderer/platform/wtf/text/text_codec_replacement.cc

#ifndef MITTO_ENCODING_TEXT_CODEC_REPLACEMENT_H_
#define MITTO_ENCODING_TEXT_CODEC_REPLACEMENT_H_

#include <stdint.h>

#include <string>

#include "base/containers/span.h"
#include "mitto/encoding/text_codec.h"

namespace mitto {

class TextCodecReplacement final : public TextCodec {
 public:
  static void RegisterEncodingNames(EncodingNameRegistrar registrar);
  static void RegisterCodecs(TextCodecRegistrar registrar);

  std::u16string Decode(base::span<const uint8_t> data,
                        FlushBehavior flush,
                        bool stop_on_error,
                        bool& saw_error) override;

 private:
  bool replacement_error_returned_ = false;
};

}  // namespace mitto

#endif  // MITTO_ENCODING_TEXT_CODEC_REPLACEMENT_H_
