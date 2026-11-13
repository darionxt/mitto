#include "mitto/encoding/text_codec_replacement.h"

#include <memory>
#include <string_view>

#include "mitto/encoding/text_codec_utf8.h"

namespace mitto {

namespace {

std::unique_ptr<TextCodec> NewReplacementDecoder(std::string_view) {
  return std::make_unique<TextCodecReplacement>();
}

}  // namespace

// static
void TextCodecReplacement::RegisterEncodingNames(
    EncodingNameRegistrar registrar) {
  // The labels of the replacement encoding in the Encoding Standard.
  for (const char* alias : {"replacement", "csiso2022kr", "hz-gb-2312",
                            "iso-2022-cn", "iso-2022-cn-ext", "iso-2022-kr"}) {
    registrar(alias, "replacement");
  }
}

// static
void TextCodecReplacement::RegisterCodecs(TextCodecRegistrar registrar) {
  registrar("replacement", &NewReplacementDecoder);
}

std::u16string TextCodecReplacement::Decode(base::span<const uint8_t> data,
                                            FlushBehavior,
                                            bool,
                                            bool& saw_error) {
  // https://encoding.spec.whatwg.org/#replacement-decoder
  if (data.empty() || replacement_error_returned_) {
    return std::u16string();
  }
  replacement_error_returned_ = true;
  saw_error = true;
  return std::u16string(1, kReplacementCharacter);
}

std::string TextCodecReplacement::Encode(std::u16string_view text,
                                         UnencodableHandling) {
  // https://encoding.spec.whatwg.org/#replacement: the encoder is UTF-8's.
  return EncodeUtf8(text);
}

}  // namespace mitto
