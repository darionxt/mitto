#include "mitto/encoding/text_codec_utf8.h"

#include <algorithm>

#include "base/check_op.h"
#include "base/strings/string_util.h"
#include "third_party/icu/source/common/unicode/utf16.h"
#include "third_party/icu/source/common/unicode/utf8.h"

namespace mitto {

namespace {

// What DecodeNonAsciiSequence() gives for an invalid sequence: the number in
// the name, and the negative of the value, is how many of its bytes were
// invalid.
constexpr int kNonCharacter1 = -1;
constexpr int kNonCharacter2 = -2;
constexpr int kNonCharacter3 = -3;

bool IsNonCharacter(int character) {
  return character >= kNonCharacter3 && character <= kNonCharacter1;
}

size_t LengthOfNonCharacter(int character) {
  DCHECK(IsNonCharacter(character));
  return static_cast<size_t>(-character);
}

bool IsAscii(uint8_t byte) {
  return byte < 0x80;
}

// The length of the sequence that each lead byte starts, or 0 for a byte that
// starts none.
constexpr std::array<uint8_t, 256> kNonAsciiSequenceLength = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    4, 4, 4, 4, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

// The code point of a sequence that starts with a byte beyond ASCII and has
// the length that its lead byte says, or a kNonCharacter value.
int DecodeNonAsciiSequence(base::span<const uint8_t> sequence) {
  DCHECK(!IsAscii(sequence[0]));
  const size_t length = sequence.size();
  if (length == 2) {
    DCHECK_GE(sequence[0], 0xC2);
    DCHECK_LE(sequence[0], 0xDF);
    if (sequence[1] < 0x80 || sequence[1] > 0xBF) {
      return kNonCharacter1;
    }
    return ((sequence[0] << 6) + sequence[1]) - 0x00003080;
  }
  if (length == 3) {
    DCHECK_GE(sequence[0], 0xE0);
    DCHECK_LE(sequence[0], 0xEF);
    switch (sequence[0]) {
      case 0xE0:
        if (sequence[1] < 0xA0 || sequence[1] > 0xBF) {
          return kNonCharacter1;
        }
        break;
      case 0xED:
        if (sequence[1] < 0x80 || sequence[1] > 0x9F) {
          return kNonCharacter1;
        }
        break;
      default:
        if (sequence[1] < 0x80 || sequence[1] > 0xBF) {
          return kNonCharacter1;
        }
    }
    if (sequence[2] < 0x80 || sequence[2] > 0xBF) {
      return kNonCharacter2;
    }
    return ((sequence[0] << 12) + (sequence[1] << 6) + sequence[2]) -
           0x000E2080;
  }
  DCHECK_EQ(length, 4u);
  DCHECK_GE(sequence[0], 0xF0);
  DCHECK_LE(sequence[0], 0xF4);
  switch (sequence[0]) {
    case 0xF0:
      if (sequence[1] < 0x90 || sequence[1] > 0xBF) {
        return kNonCharacter1;
      }
      break;
    case 0xF4:
      if (sequence[1] < 0x80 || sequence[1] > 0x8F) {
        return kNonCharacter1;
      }
      break;
    default:
      if (sequence[1] < 0x80 || sequence[1] > 0xBF) {
        return kNonCharacter1;
      }
  }
  if (sequence[2] < 0x80 || sequence[2] > 0xBF) {
    return kNonCharacter2;
  }
  if (sequence[3] < 0x80 || sequence[3] > 0xBF) {
    return kNonCharacter3;
  }
  return ((sequence[0] << 18) + (sequence[1] << 12) + (sequence[2] << 6) +
          sequence[3]) -
         0x03C82080;
}

}  // namespace

// static
std::unique_ptr<TextCodec> TextCodecUtf8::Create(std::string_view) {
  return std::make_unique<TextCodecUtf8>();
}

// static
bool TextCodecUtf8::IsSupported(std::string_view canonical_name) {
  return base::EqualsCaseInsensitiveASCII(canonical_name, "UTF-8");
}

// static
void TextCodecUtf8::RegisterEncodingNames(EncodingNameRegistrar registrar) {
  registrar("UTF-8", "UTF-8");
  registrar("unicode11utf8", "UTF-8");
  registrar("unicode20utf8", "UTF-8");
  registrar("utf8", "UTF-8");
  registrar("x-unicode20utf8", "UTF-8");
  registrar("unicode-1-1-utf-8", "UTF-8");
}

// static
void TextCodecUtf8::RegisterCodecs(TextCodecRegistrar registrar) {
  registrar("UTF-8", &Create);
}

TextCodecUtf8::TextCodecUtf8() = default;
TextCodecUtf8::~TextCodecUtf8() = default;

void TextCodecUtf8::SavePartialSequenceBytes(
    base::span<const uint8_t>& source) {
  DCHECK(!partial_sequence_size_);
  partial_sequence_size_ = source.size();
  base::span(partial_sequence_)
      .first(partial_sequence_size_)
      .copy_from(source.take_first(partial_sequence_size_));
}

void TextCodecUtf8::ConsumePartialSequenceBytes(size_t num_bytes) {
  DCHECK_GE(partial_sequence_size_, num_bytes);
  partial_sequence_size_ -= num_bytes;
  base::span(partial_sequence_)
      .first(partial_sequence_size_)
      .copy_from(base::span(partial_sequence_)
                     .subspan(num_bytes, partial_sequence_size_));
}

void TextCodecUtf8::FillPartialSequenceBytes(
    size_t sequence_length,
    base::span<const uint8_t>& source) {
  DCHECK_GT(sequence_length, partial_sequence_size_);
  if (!source.empty()) {
    const size_t additional_bytes = std::min<size_t>(
        sequence_length - partial_sequence_size_, source.size());
    base::span(partial_sequence_)
        .subspan(partial_sequence_size_, additional_bytes)
        .copy_from(source.take_first(additional_bytes));
    partial_sequence_size_ += additional_bytes;
  }
  // Without enough bytes, the rest are zeros, which are not part of the
  // sequence, so that DecodeNonAsciiSequence() tells whether what there is is
  // valid so far.
  if (sequence_length > partial_sequence_size_) {
    std::ranges::fill(base::span(partial_sequence_)
                          .first(sequence_length)
                          .last(sequence_length - partial_sequence_size_),
                      0);
  }
}

bool TextCodecUtf8::NeedMoreData(size_t sequence_length,
                                 int character,
                                 bool flush) const {
  if (sequence_length <= partial_sequence_size_) {
    return false;
  }
  if (flush) {
    return false;
  }
  // The sequence so far is incomplete but valid, which is not an error yet.
  return LengthOfNonCharacter(character) == partial_sequence_size_;
}

void TextCodecUtf8::HandlePartialSequence(std::u16string& destination,
                                          base::span<const uint8_t>& source,
                                          bool flush,
                                          bool stop_on_error,
                                          bool& saw_error) {
  DCHECK(partial_sequence_size_);
  do {
    if (IsAscii(partial_sequence_[0])) {
      destination.push_back(partial_sequence_[0]);
      ConsumePartialSequenceBytes(1);
      continue;
    }
    size_t count = kNonAsciiSequenceLength[partial_sequence_[0]];
    int character;
    if (!count) {
      character = kNonCharacter1;
    } else {
      if (count > partial_sequence_size_) {
        FillPartialSequenceBytes(count, source);
      }
      character =
          DecodeNonAsciiSequence(base::span(partial_sequence_).first(count));
      if (NeedMoreData(count, character, flush)) {
        return;
      }
    }
    if (IsNonCharacter(character)) {
      saw_error = true;
      if (stop_on_error) {
        return;
      }
      count = LengthOfNonCharacter(character);
      character = kReplacementCharacter;
    }
    AppendCodePoint(destination, character);
    ConsumePartialSequenceBytes(count);
  } while (partial_sequence_size_);
}

std::u16string TextCodecUtf8::Decode(base::span<const uint8_t> bytes,
                                     FlushBehavior flush,
                                     bool stop_on_error,
                                     bool& saw_error) {
  const bool do_flush = flush != FlushBehavior::kDoNotFlush;
  std::u16string result;
  result.reserve(partial_sequence_size_ + bytes.size());
  base::span<const uint8_t> source = bytes;
  do {
    if (partial_sequence_size_) {
      HandlePartialSequence(result, source, do_flush, stop_on_error, saw_error);
      if (partial_sequence_size_) {
        break;
      }
    }
    while (!source.empty()) {
      if (IsAscii(source[0])) {
        result.push_back(source.take_first_elem());
        continue;
      }
      size_t count = kNonAsciiSequenceLength[source[0]];
      int character;
      if (count == 0) {
        character = kNonCharacter1;
      } else {
        if (count > source.size()) {
          SavePartialSequenceBytes(source);
          break;
        }
        character = DecodeNonAsciiSequence(source.first(count));
      }
      if (IsNonCharacter(character)) {
        saw_error = true;
        if (stop_on_error) {
          break;
        }
        // One U+FFFD for the longest start of a sequence that could have been
        // valid, the "maximal subpart".
        count = LengthOfNonCharacter(character);
        character = kReplacementCharacter;
      }
      source = source.subspan(count);
      AppendCodePoint(result, character);
    }
  } while (partial_sequence_size_);
  return result;
}

std::string EncodeUtf8(std::u16string_view text) {
  // At most three bytes for each code unit: a code point beyond the Basic
  // Multilingual Plane takes two code units and four bytes.
  std::string bytes(text.size() * 3, '\0');
  const size_t length = text.size();
  size_t i = 0;
  size_t bytes_written = 0;
  while (i < length) {
    UChar32 character;
    U16_NEXT(text, i, length, character);
    // U16_NEXT() gives a lone surrogate as it is.
    if (U_IS_SURROGATE(character)) {
      character = kReplacementCharacter;
    }
    U8_APPEND_UNSAFE(bytes, bytes_written, character);
  }
  bytes.resize(bytes_written);
  return bytes;
}

Utf8EncodeIntoResult EncodeUtf8Into(std::u16string_view text,
                                    base::span<uint8_t> destination) {
  const size_t length = text.size();
  Utf8EncodeIntoResult result;
  size_t i = 0;
  size_t previous_code_unit_index = 0;
  bool is_error = false;
  while (i < length && result.bytes_written < destination.size() && !is_error) {
    UChar32 character;
    previous_code_unit_index = i;
    U16_NEXT(text, i, length, character);
    if (U_IS_SURROGATE(character)) {
      character = kReplacementCharacter;
    }
    U8_APPEND(destination, result.bytes_written, destination.size(), character,
              is_error);
  }
  // U8_APPEND() fails only when the code point's bytes do not fit, and then
  // the code point was not used.
  result.code_units_read = is_error ? previous_code_unit_index : i;
  return result;
}

}  // namespace mitto
