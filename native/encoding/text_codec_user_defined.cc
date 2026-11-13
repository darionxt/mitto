#include "mitto/encoding/text_codec_user_defined.h"

#include <memory>
#include <string_view>

namespace mitto {

namespace {

std::unique_ptr<TextCodec> NewUserDefinedDecoder(std::string_view) {
  return std::make_unique<TextCodecUserDefined>();
}

}  // namespace

// static
void TextCodecUserDefined::RegisterEncodingNames(
    EncodingNameRegistrar registrar) {
  registrar("x-user-defined", "x-user-defined");
}

// static
void TextCodecUserDefined::RegisterCodecs(TextCodecRegistrar registrar) {
  registrar("x-user-defined", &NewUserDefinedDecoder);
}

std::u16string TextCodecUserDefined::Decode(base::span<const uint8_t> data,
                                            FlushBehavior,
                                            bool,
                                            bool&) {
  std::u16string result;
  result.reserve(data.size());
  for (const uint8_t byte : data) {
    // The byte sign-extended, as Blink reads it through a signed char.
    const int extended = static_cast<int8_t>(byte);
    result.push_back(static_cast<char16_t>(extended & 0xF7FF));
  }
  return result;
}

}  // namespace mitto
