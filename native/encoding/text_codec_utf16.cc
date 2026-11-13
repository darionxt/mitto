#include "mitto/encoding/text_codec_utf16.h"

#include <memory>

#include "base/check.h"
#include "base/strings/string_util.h"
#include "third_party/icu/source/common/unicode/utf16.h"

namespace mitto {

namespace {

std::unique_ptr<TextCodec> NewUtf16LittleEndianDecoder(std::string_view) {
  return std::make_unique<TextCodecUtf16>(/*little_endian=*/true);
}

std::unique_ptr<TextCodec> NewUtf16BigEndianDecoder(std::string_view) {
  return std::make_unique<TextCodecUtf16>(/*little_endian=*/false);
}

}  // namespace

// static
bool TextCodecUtf16::IsSupported(std::string_view canonical_name) {
  return base::EqualsCaseInsensitiveASCII(canonical_name, "UTF-16LE") ||
         base::EqualsCaseInsensitiveASCII(canonical_name, "UTF-16BE");
}

// static
void TextCodecUtf16::RegisterEncodingNames(EncodingNameRegistrar registrar) {
  registrar("UTF-16LE", "UTF-16LE");
  registrar("UTF-16BE", "UTF-16BE");
  registrar("ISO-10646-UCS-2", "UTF-16LE");
  registrar("UCS-2", "UTF-16LE");
  registrar("UTF-16", "UTF-16LE");
  registrar("Unicode", "UTF-16LE");
  registrar("csUnicode", "UTF-16LE");
  registrar("unicodeFEFF", "UTF-16LE");
  registrar("unicodeFFFE", "UTF-16BE");
}

// static
void TextCodecUtf16::RegisterCodecs(TextCodecRegistrar registrar) {
  registrar("UTF-16LE", &NewUtf16LittleEndianDecoder);
  registrar("UTF-16BE", &NewUtf16BigEndianDecoder);
}

TextCodecUtf16::TextCodecUtf16(bool little_endian)
    : little_endian_(little_endian) {}

TextCodecUtf16::~TextCodecUtf16() = default;

std::u16string TextCodecUtf16::Decode(base::span<const uint8_t> bytes,
                                      FlushBehavior flush,
                                      bool,
                                      bool& saw_error) {
  // The end of a fetch's body is not the end here, as in Blink.
  const bool really_flush =
      flush != FlushBehavior::kDoNotFlush && flush != FlushBehavior::kFetchEof;

  std::u16string result;
  if (bytes.empty()) {
    if (really_flush && (have_lead_byte_ || have_lead_surrogate_)) {
      have_lead_byte_ = have_lead_surrogate_ = false;
      saw_error = true;
      result.push_back(kReplacementCharacter);
    }
    return result;
  }

  const size_t num_bytes = bytes.size() + (have_lead_byte_ ? 1 : 0);
  const bool will_have_extra_byte = num_bytes & 1;
  size_t num_chars_in = num_bytes / 2;
  result.reserve(num_chars_in + 2);

  auto decode = [&](char16_t c) {
    if (have_lead_surrogate_ && U16_IS_TRAIL(c)) {
      result.push_back(lead_surrogate_);
      have_lead_surrogate_ = false;
      result.push_back(c);
      return;
    }
    if (have_lead_surrogate_) {
      have_lead_surrogate_ = false;
      saw_error = true;
      result.push_back(kReplacementCharacter);
    }
    if (U16_IS_LEAD(c)) {
      have_lead_surrogate_ = true;
      lead_surrogate_ = c;
    } else if (U16_IS_TRAIL(c)) {
      saw_error = true;
      result.push_back(kReplacementCharacter);
    } else {
      result.push_back(c);
    }
  };
  auto unit = [this](uint8_t first, uint8_t second) {
    return little_endian_ ? static_cast<char16_t>(first | (second << 8))
                          : static_cast<char16_t>((first << 8) | second);
  };

  base::span<const uint8_t> in = bytes;
  if (have_lead_byte_) {
    have_lead_byte_ = false;
    decode(unit(lead_byte_, in.take_first_elem()));
    num_chars_in--;
  }
  for (size_t i = 0; i < num_chars_in; ++i) {
    const auto pair = in.take_first<2u>();
    decode(unit(pair[0], pair[1]));
  }
  DCHECK(!have_lead_byte_);
  if (will_have_extra_byte) {
    have_lead_byte_ = true;
    lead_byte_ = in[0];
  }

  if (really_flush && (have_lead_byte_ || have_lead_surrogate_)) {
    have_lead_byte_ = have_lead_surrogate_ = false;
    saw_error = true;
    result.push_back(kReplacementCharacter);
  }
  return result;
}

std::string TextCodecUtf16::Encode(std::u16string_view text,
                                   UnencodableHandling) {
  // Each code unit as it is, a lone surrogate too, as in Blink.
  std::string result(text.size() * 2, '\0');
  for (size_t i = 0; i < text.size(); ++i) {
    const char16_t c = text[i];
    const auto low = static_cast<char>(c);
    const auto high = static_cast<char>(c >> 8);
    result[i * 2] = little_endian_ ? low : high;
    result[i * 2 + 1] = little_endian_ ? high : low;
  }
  return result;
}

}  // namespace mitto
