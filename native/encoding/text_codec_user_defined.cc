#include "mitto/encoding/text_codec_user_defined.h"

#include <memory>
#include <string_view>

#include "base/check_op.h"
#include "third_party/icu/source/common/unicode/utf16.h"

namespace mitto {

namespace {

std::unique_ptr<TextCodec> NewUserDefinedDecoder(std::string_view) {
  return std::make_unique<TextCodecUserDefined>();
}

// Encodes a string that has a character beyond ASCII.
std::string EncodeComplexUserDefined(std::u16string_view text,
                                     UnencodableHandling handling) {
  DCHECK_NE(handling, UnencodableHandling::kNone);
  std::string result;
  result.reserve(text.size());
  const size_t length = text.size();
  size_t i = 0;
  while (i < length) {
    UChar32 code_point;
    U16_NEXT(text, i, length, code_point);
    // The byte that decodes to the code point, if any, as Decode() reads it
    // through a signed char.
    const auto signed_byte = static_cast<int8_t>(code_point);
    if ((signed_byte & 0xF7FF) == code_point) {
      result.push_back(static_cast<char>(signed_byte));
      continue;
    }
    const std::string replacement =
        TextCodec::GetUnencodableReplacement(code_point, handling);
    DCHECK(!replacement.empty());
    result.append(replacement);
  }
  return result;
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

std::string TextCodecUserDefined::Encode(std::u16string_view text,
                                         UnencodableHandling handling) {
  // Copies each code unit's low byte, and checks at once whether all are
  // ASCII, which most strings are.
  std::string result(text.size(), '\0');
  char16_t ored = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    result[i] = static_cast<char>(text[i]);
    ored |= text[i];
  }
  if (!(ored & 0xFF80)) {
    return result;
  }
  return EncodeComplexUserDefined(text, handling);
}

}  // namespace mitto
