#include "mitto/encoding/text_codec_cjk.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

#include "base/check_op.h"
#include "base/memory/raw_ptr.h"
#include "base/notreached.h"
#include "base/strings/string_util.h"
#include "mitto/encoding/encoding_tables.h"
#include "third_party/icu/source/common/unicode/utf16.h"

namespace mitto {

// The state of one stream: the lead byte of a sequence, and a byte that an
// error gives back to be read again.
class TextCodecCjk::Decoder {
 public:
  virtual ~Decoder() = default;
  virtual std::u16string Decode(base::span<const uint8_t> bytes,
                                bool flush,
                                bool stop_on_error,
                                bool& saw_error);

 protected:
  enum class SawError { kNo, kYes };
  virtual SawError ParseByte(uint8_t byte, std::u16string& result) = 0;
  virtual void Finalize(bool flush, std::u16string& result) {}

  uint8_t lead_ = 0x00;
  std::optional<uint8_t> prepended_byte_;
};

namespace {

constexpr char kCanonicalNameEucJp[] = "EUC-JP";
constexpr char kCanonicalNameShiftJis[] = "Shift_JIS";
constexpr char kCanonicalNameEucKr[] = "EUC-KR";
constexpr char kCanonicalNameIso2022Jp[] = "ISO-2022-JP";
constexpr char kCanonicalNameGbk[] = "GBK";
constexpr char kCanonicalNameGb18030[] = "gb18030";
constexpr char kCanonicalNameBig5[] = "Big5";
constexpr char kCanonicalNameBig5Hkscs[] = "Big5-HKSCS";

constexpr std::array<const char*, 8> kSupportedCanonicalNames = {
    kCanonicalNameEucJp,     kCanonicalNameShiftJis,  kCanonicalNameEucKr,
    kCanonicalNameIso2022Jp, kCanonicalNameGbk,       kCanonicalNameGb18030,
    kCanonicalNameBig5,      kCanonicalNameBig5Hkscs,
};

constexpr char16_t kYenSign = 0x00A5;
constexpr char16_t kOverline = 0x203E;
constexpr char16_t kMinusSign = 0x2212;

bool IsAscii(uint8_t byte) {
  return byte < 0x80;
}

// https://encoding.spec.whatwg.org/index-gb18030-ranges.txt
constexpr std::array<std::pair<uint32_t, int32_t>, 207> kGb18030Ranges = {{
    {0, 0x0080},     {36, 0x00A5},    {38, 0x00A9},      {45, 0x00B2},
    {50, 0x00B8},    {81, 0x00D8},    {89, 0x00E2},      {95, 0x00EB},
    {96, 0x00EE},    {100, 0x00F4},   {103, 0x00F8},     {104, 0x00FB},
    {105, 0x00FD},   {109, 0x0102},   {126, 0x0114},     {133, 0x011C},
    {148, 0x012C},   {172, 0x0145},   {175, 0x0149},     {179, 0x014E},
    {208, 0x016C},   {306, 0x01CF},   {307, 0x01D1},     {308, 0x01D3},
    {309, 0x01D5},   {310, 0x01D7},   {311, 0x01D9},     {312, 0x01DB},
    {313, 0x01DD},   {341, 0x01FA},   {428, 0x0252},     {443, 0x0262},
    {544, 0x02C8},   {545, 0x02CC},   {558, 0x02DA},     {741, 0x03A2},
    {742, 0x03AA},   {749, 0x03C2},   {750, 0x03CA},     {805, 0x0402},
    {819, 0x0450},   {820, 0x0452},   {7922, 0x2011},    {7924, 0x2017},
    {7925, 0x201A},  {7927, 0x201E},  {7934, 0x2027},    {7943, 0x2031},
    {7944, 0x2034},  {7945, 0x2036},  {7950, 0x203C},    {8062, 0x20AD},
    {8148, 0x2104},  {8149, 0x2106},  {8152, 0x210A},    {8164, 0x2117},
    {8174, 0x2122},  {8236, 0x216C},  {8240, 0x217A},    {8262, 0x2194},
    {8264, 0x219A},  {8374, 0x2209},  {8380, 0x2210},    {8381, 0x2212},
    {8384, 0x2216},  {8388, 0x221B},  {8390, 0x2221},    {8392, 0x2224},
    {8393, 0x2226},  {8394, 0x222C},  {8396, 0x222F},    {8401, 0x2238},
    {8406, 0x223E},  {8416, 0x2249},  {8419, 0x224D},    {8424, 0x2253},
    {8437, 0x2262},  {8439, 0x2268},  {8445, 0x2270},    {8482, 0x2296},
    {8485, 0x229A},  {8496, 0x22A6},  {8521, 0x22C0},    {8603, 0x2313},
    {8936, 0x246A},  {8946, 0x249C},  {9046, 0x254C},    {9050, 0x2574},
    {9063, 0x2590},  {9066, 0x2596},  {9076, 0x25A2},    {9092, 0x25B4},
    {9100, 0x25BE},  {9108, 0x25C8},  {9111, 0x25CC},    {9113, 0x25D0},
    {9131, 0x25E6},  {9162, 0x2607},  {9164, 0x260A},    {9218, 0x2641},
    {9219, 0x2643},  {11329, 0x2E82}, {11331, 0x2E85},   {11334, 0x2E89},
    {11336, 0x2E8D}, {11346, 0x2E98}, {11361, 0x2EA8},   {11363, 0x2EAB},
    {11366, 0x2EAF}, {11370, 0x2EB4}, {11372, 0x2EB8},   {11375, 0x2EBC},
    {11389, 0x2ECB}, {11682, 0x2FFC}, {11686, 0x3004},   {11687, 0x3018},
    {11692, 0x301F}, {11694, 0x302A}, {11714, 0x303F},   {11716, 0x3094},
    {11723, 0x309F}, {11725, 0x30F7}, {11730, 0x30FF},   {11736, 0x312A},
    {11982, 0x322A}, {11989, 0x3232}, {12102, 0x32A4},   {12336, 0x3390},
    {12348, 0x339F}, {12350, 0x33A2}, {12384, 0x33C5},   {12393, 0x33CF},
    {12395, 0x33D3}, {12397, 0x33D6}, {12510, 0x3448},   {12553, 0x3474},
    {12851, 0x359F}, {12962, 0x360F}, {12973, 0x361B},   {13738, 0x3919},
    {13823, 0x396F}, {13919, 0x39D1}, {13933, 0x39E0},   {14080, 0x3A74},
    {14298, 0x3B4F}, {14585, 0x3C6F}, {14698, 0x3CE1},   {15583, 0x4057},
    {15847, 0x4160}, {16318, 0x4338}, {16434, 0x43AD},   {16438, 0x43B2},
    {16481, 0x43DE}, {16729, 0x44D7}, {17102, 0x464D},   {17122, 0x4662},
    {17315, 0x4724}, {17320, 0x472A}, {17402, 0x477D},   {17418, 0x478E},
    {17859, 0x4948}, {17909, 0x497B}, {17911, 0x497E},   {17915, 0x4984},
    {17916, 0x4987}, {17936, 0x499C}, {17939, 0x49A0},   {17961, 0x49B8},
    {18664, 0x4C78}, {18703, 0x4CA4}, {18814, 0x4D1A},   {18962, 0x4DAF},
    {19043, 0x9FA6}, {33469, 0xE76C}, {33470, 0xE7C8},   {33471, 0xE7E7},
    {33484, 0xE815}, {33485, 0xE819}, {33490, 0xE81F},   {33497, 0xE827},
    {33501, 0xE82D}, {33505, 0xE833}, {33513, 0xE83C},   {33520, 0xE844},
    {33536, 0xE856}, {33550, 0xE865}, {37845, 0xF92D},   {37921, 0xF97A},
    {37948, 0xF996}, {38029, 0xF9E8}, {38038, 0xF9F2},   {38064, 0xFA10},
    {38065, 0xFA12}, {38066, 0xFA15}, {38069, 0xFA19},   {38075, 0xFA22},
    {38076, 0xFA25}, {38078, 0xFA2A}, {39108, 0xFE32},   {39109, 0xFE45},
    {39113, 0xFE53}, {39114, 0xFE58}, {39115, 0xFE67},   {39116, 0xFE6C},
    {39265, 0xFF5F}, {39394, 0xFFE6}, {189000, 0x10000},
}};

// https://encoding.spec.whatwg.org/#index-gb18030-ranges-code-point
std::optional<int32_t> IndexGb18030RangesCodePoint(uint32_t pointer) {
  if ((pointer > 39419 && pointer < 189000) || pointer > 1237575) {
    return std::nullopt;
  }
  if (pointer == 7457) {
    return 0xE7C7;
  }
  const auto upper_bound = std::ranges::upper_bound(
      kGb18030Ranges, pointer, {}, &std::pair<uint32_t, int32_t>::first);
  DCHECK(upper_bound != kGb18030Ranges.begin());
  const auto& [offset, code_point_offset] = *std::prev(upper_bound);
  return code_point_offset + static_cast<int32_t>(pointer - offset);
}

// https://encoding.spec.whatwg.org/#index-gb18030-ranges-pointer
uint32_t Gb18030RangesPointer(UChar32 code_point) {
  if (code_point == 0xE7C7) {
    return 7457;
  }
  const auto upper_bound = std::ranges::upper_bound(
      kGb18030Ranges, code_point, {}, &std::pair<uint32_t, int32_t>::second);
  DCHECK(upper_bound != kGb18030Ranges.begin());
  const auto& [pointer_offset, offset] = *std::prev(upper_bound);
  return pointer_offset + static_cast<uint32_t>(code_point - offset);
}

bool IsAsciiCodePoint(UChar32 code_point) {
  return code_point >= 0 && code_point < 0x80;
}

void AppendByte(std::string& result, uint32_t byte) {
  result.push_back(static_cast<char>(byte));
}

void AppendUnencodableReplacement(UChar32 code_point,
                                  UnencodableHandling handling,
                                  std::string& result) {
  result.append(TextCodec::GetUnencodableReplacement(code_point, handling));
}

// Calls `encode` with each code point of `text`, and with a lone surrogate as
// it is, as Blink's CodePointIterator gives them.
template <typename Function>
void ForEachCodePoint(std::u16string_view text, Function encode) {
  const size_t length = text.size();
  size_t i = 0;
  while (i < length) {
    UChar32 code_point;
    U16_NEXT(text, i, length, code_point);
    encode(code_point);
  }
}

// https://encoding.spec.whatwg.org/#euc-jp-encoder
std::string EncodeEucJp(std::u16string_view text,
                        UnencodableHandling handling) {
  std::string result;
  result.reserve(text.size());
  ForEachCodePoint(text, [&](UChar32 code_point) {
    if (IsAsciiCodePoint(code_point)) {
      AppendByte(result, code_point);
      return;
    }
    if (code_point == kYenSign) {
      AppendByte(result, 0x5C);
      return;
    }
    if (code_point == kOverline) {
      AppendByte(result, 0x7E);
      return;
    }
    if (code_point >= 0xFF61 && code_point <= 0xFF9F) {
      AppendByte(result, 0x8E);
      AppendByte(result, code_point - 0xFF61 + 0xA1);
      return;
    }
    if (code_point == kMinusSign) {
      code_point = 0xFF0D;
    }
    const std::optional<uint16_t> pointer =
        FindPointer(Jis0208EncodeIndex(), code_point);
    if (!pointer) {
      AppendUnencodableReplacement(code_point, handling, result);
      return;
    }
    AppendByte(result, *pointer / 94 + 0xA1);
    AppendByte(result, *pointer % 94 + 0xA1);
  });
  return result;
}

// https://encoding.spec.whatwg.org/#iso-2022-jp-encoder
class Iso2022JpEncoder {
 public:
  static std::string Encode(std::u16string_view text,
                            UnencodableHandling handling) {
    Iso2022JpEncoder encoder(handling, text.size());
    ForEachCodePoint(
        text, [&](UChar32 code_point) { encoder.ParseCodePoint(code_point); });
    return encoder.Finalize();
  }

 private:
  enum class State : uint8_t { kAscii, kRoman, kJis0208 };

  // https://encoding.spec.whatwg.org/index-iso-2022-jp-katakana.txt
  static constexpr std::array<char16_t, 63> kIso2022JpKatakana = {
      0x3002, 0x300C, 0x300D, 0x3001, 0x30FB, 0x30F2, 0x30A1, 0x30A3, 0x30A5,
      0x30A7, 0x30A9, 0x30E3, 0x30E5, 0x30E7, 0x30C3, 0x30FC, 0x30A2, 0x30A4,
      0x30A6, 0x30A8, 0x30AA, 0x30AB, 0x30AD, 0x30AF, 0x30B1, 0x30B3, 0x30B5,
      0x30B7, 0x30B9, 0x30BB, 0x30BD, 0x30BF, 0x30C1, 0x30C4, 0x30C6, 0x30C8,
      0x30CA, 0x30CB, 0x30CC, 0x30CD, 0x30CE, 0x30CF, 0x30D2, 0x30D5, 0x30D8,
      0x30DB, 0x30DE, 0x30DF, 0x30E0, 0x30E1, 0x30E2, 0x30E4, 0x30E6, 0x30E8,
      0x30E9, 0x30EA, 0x30EB, 0x30EC, 0x30ED, 0x30EF, 0x30F3, 0x309B, 0x309C};
  static_assert(kIso2022JpKatakana.size() == 0xFF9F - 0xFF61 + 1);

  Iso2022JpEncoder(UnencodableHandling handling, size_t length)
      : handling_(handling) {
    result_.reserve(length);
  }

  void ChangeStateToAscii() {
    result_.append("\x1B\x28\x42");
    state_ = State::kAscii;
  }

  void ChangeStateToRoman() {
    result_.append("\x1B\x28\x4A");
    state_ = State::kRoman;
  }

  void ChangeStateToJis0208() {
    result_.append("\x1B\x24\x42");
    state_ = State::kJis0208;
  }

  void ParseCodePoint(UChar32 code_point) {
    if ((state_ == State::kAscii || state_ == State::kRoman) &&
        (code_point == 0x000E || code_point == 0x000F ||
         code_point == 0x001B)) {
      StatefulUnencodableHandler(kReplacementCharacter);
      return;
    }
    if (state_ == State::kAscii && IsAsciiCodePoint(code_point)) {
      AppendByte(result_, code_point);
      return;
    }
    if (state_ == State::kRoman) {
      if (IsAsciiCodePoint(code_point) && code_point != 0x005C &&
          code_point != 0x007E) {
        AppendByte(result_, code_point);
        return;
      }
      if (code_point == kYenSign) {
        AppendByte(result_, 0x5C);
        return;
      }
      if (code_point == kOverline) {
        AppendByte(result_, 0x7E);
        return;
      }
    }
    if (IsAsciiCodePoint(code_point) && state_ != State::kAscii) {
      ChangeStateToAscii();
      ParseCodePoint(code_point);
      return;
    }
    if ((code_point == kYenSign || code_point == kOverline) &&
        state_ != State::kRoman) {
      ChangeStateToRoman();
      ParseCodePoint(code_point);
      return;
    }
    if (code_point == kMinusSign) {
      code_point = 0xFF0D;
    }
    if (code_point >= 0xFF61 && code_point <= 0xFF9F) {
      code_point = kIso2022JpKatakana[static_cast<size_t>(code_point - 0xFF61)];
    }
    const std::optional<uint16_t> pointer =
        FindPointer(Jis0208EncodeIndex(), code_point);
    if (!pointer) {
      StatefulUnencodableHandler(code_point);
      return;
    }
    if (state_ != State::kJis0208) {
      ChangeStateToJis0208();
      ParseCodePoint(code_point);
      return;
    }
    AppendByte(result_, *pointer / 94 + 0x21);
    AppendByte(result_, *pointer % 94 + 0x21);
  }

  std::string Finalize() {
    if (state_ != State::kAscii) {
      ChangeStateToAscii();
    }
    return std::move(result_);
  }

  // Goes back to ASCII from JIS X 0208, so that the replacement's ASCII
  // characters read as themselves.
  void StatefulUnencodableHandler(UChar32 code_point) {
    if (state_ == State::kJis0208) {
      ChangeStateToAscii();
    }
    AppendUnencodableReplacement(code_point, handling_, result_);
  }

  const UnencodableHandling handling_;
  State state_ = State::kAscii;
  std::string result_;
};

// https://encoding.spec.whatwg.org/#shift_jis-encoder
std::string EncodeShiftJis(std::u16string_view text,
                           UnencodableHandling handling) {
  std::string result;
  result.reserve(text.size());
  ForEachCodePoint(text, [&](UChar32 code_point) {
    if (IsAsciiCodePoint(code_point) || code_point == 0x0080) {
      AppendByte(result, code_point);
      return;
    }
    if (code_point == kYenSign) {
      AppendByte(result, 0x5C);
      return;
    }
    if (code_point == kOverline) {
      AppendByte(result, 0x7E);
      return;
    }
    if (code_point >= 0xFF61 && code_point <= 0xFF9F) {
      AppendByte(result, code_point - 0xFF61 + 0xA1);
      return;
    }
    if (code_point == kMinusSign) {
      code_point = 0xFF0D;
    }
    const base::span<const std::pair<char16_t, uint16_t>> pointers =
        FindPointers(Jis0208EncodeIndex(), code_point);
    if (pointers.empty()) {
      AppendUnencodableReplacement(code_point, handling, result);
      return;
    }
    DCHECK_LE(pointers.size(), 3u);
    for (const auto& entry : pointers) {
      const uint16_t pointer = entry.second;
      // The index Shift_JIS pointer leaves out pointers 8272 to 8835. Blink
      // writes nothing for a code point that has only those, as this does.
      if (pointer >= 8272 && pointer <= 8835) {
        continue;
      }
      const uint32_t lead = pointer / 188;
      const uint32_t lead_offset = lead < 0x1F ? 0x81 : 0xC1;
      const uint32_t trail = pointer % 188;
      const uint32_t offset = trail < 0x3F ? 0x40 : 0x41;
      AppendByte(result, lead + lead_offset);
      AppendByte(result, trail + offset);
      break;
    }
  });
  return result;
}

// https://encoding.spec.whatwg.org/#euc-kr-encoder
std::string EncodeEucKr(std::u16string_view text,
                        UnencodableHandling handling) {
  std::string result;
  result.reserve(text.size());
  ForEachCodePoint(text, [&](UChar32 code_point) {
    if (IsAsciiCodePoint(code_point)) {
      AppendByte(result, code_point);
      return;
    }
    const std::optional<uint16_t> pointer =
        FindPointer(EucKrEncodeIndex(), code_point);
    if (!pointer) {
      AppendUnencodableReplacement(code_point, handling, result);
      return;
    }
    AppendByte(result, *pointer / 190 + 0x81);
    AppendByte(result, *pointer % 190 + 0x41);
  });
  return result;
}

// Code points that gb18030 encodes with two bytes that decode to other code
// points since GB18030-2022, as Blink has them
// (https://unicode-org.atlassian.net/browse/ICU-22357). The index by code point
// no longer has them, and the four-byte ranges would take them otherwise.
std::optional<uint16_t> Gb18030AsymmetricEncode(UChar32 code_point) {
  switch (code_point) {
    case 0xE81E:
      return 0xFE59;
    case 0xE826:
      return 0xFE61;
    case 0xE82B:
      return 0xFE66;
    case 0xE82C:
      return 0xFE67;
    case 0xE832:
      return 0xFE6D;
    case 0xE843:
      return 0xFE7E;
    case 0xE854:
      return 0xFE90;
    case 0xE864:
      return 0xFEA0;
    case 0xE78D:
      return 0xA6D9;
    case 0xE78F:
      return 0xA6DB;
    case 0xE78E:
      return 0xA6DA;
    case 0xE790:
      return 0xA6DC;
    case 0xE791:
      return 0xA6DD;
    case 0xE792:
      return 0xA6DE;
    case 0xE793:
      return 0xA6DF;
    case 0xE794:
      return 0xA6EC;
    case 0xE795:
      return 0xA6ED;
    case 0xE796:
      return 0xA6F3;
  }
  return std::nullopt;
}

// https://encoding.spec.whatwg.org/#gb18030-encoder, which GBK encodes with
// too, without the four-byte sequences.
enum class IsGbk : bool { kNo, kYes };
std::string EncodeGbShared(std::u16string_view text,
                           UnencodableHandling handling,
                           IsGbk is_gbk) {
  std::string result;
  result.reserve(text.size());
  ForEachCodePoint(text, [&](UChar32 code_point) {
    if (IsAsciiCodePoint(code_point)) {
      AppendByte(result, code_point);
      return;
    }
    if (code_point == 0xE5E5) {
      AppendUnencodableReplacement(code_point, handling, result);
      return;
    }
    if (is_gbk == IsGbk::kYes && code_point == 0x20AC) {
      AppendByte(result, 0x80);
      return;
    }
    if (const std::optional<uint16_t> encoded =
            Gb18030AsymmetricEncode(code_point)) {
      AppendByte(result, *encoded >> 8);
      AppendByte(result, *encoded & 0xFF);
      return;
    }
    if (const std::optional<uint16_t> pointer =
            FindPointer(Gb18030EncodeIndex(), code_point)) {
      const uint32_t lead = *pointer / 190 + 0x81;
      const uint32_t trail = *pointer % 190;
      const uint32_t offset = trail < 0x3F ? 0x40 : 0x41;
      AppendByte(result, lead);
      AppendByte(result, trail + offset);
      return;
    }
    if (is_gbk == IsGbk::kYes) {
      AppendUnencodableReplacement(code_point, handling, result);
      return;
    }
    uint32_t pointer = Gb18030RangesPointer(code_point);
    const uint32_t byte1 = pointer / (10 * 126 * 10);
    pointer = pointer % (10 * 126 * 10);
    const uint32_t byte2 = pointer / (10 * 126);
    pointer = pointer % (10 * 126);
    const uint32_t byte3 = pointer / 10;
    const uint32_t byte4 = pointer % 10;
    AppendByte(result, byte1 + 0x81);
    AppendByte(result, byte2 + 0x30);
    AppendByte(result, byte3 + 0x81);
    AppendByte(result, byte4 + 0x30);
  });
  return result;
}

// https://encoding.spec.whatwg.org/#big5-encoder
std::string EncodeBig5(std::u16string_view text, UnencodableHandling handling) {
  std::string result;
  result.reserve(text.size());
  ForEachCodePoint(text, [&](UChar32 code_point) {
    if (IsAsciiCodePoint(code_point)) {
      AppendByte(result, code_point);
      return;
    }
    // Blink takes U+224C as a code point that Big5 lacks, as the Encoding
    // Standard's encoder does.
    if (code_point == 0x224C) {
      AppendUnencodableReplacement(code_point, handling, result);
      return;
    }
    if (const std::optional<uint16_t> pointer =
            FindPointer(Big5EncodeIndex(), code_point)) {
      AppendByte(result, *pointer / 157 + 0x81);
      const uint32_t trail = *pointer % 157;
      AppendByte(result, trail + (trail < 0x3F ? 0x40 : 0x62));
      return;
    }
    AppendUnencodableReplacement(code_point, handling, result);
  });
  return result;
}

// https://encoding.spec.whatwg.org/#euc-jp-decoder
class EucJpDecoder : public TextCodecCjk::Decoder {
 protected:
  void Finalize(bool flush, std::u16string&) override {
    if (flush) {
      // The selection of JIS X 0212 does not go on to the next stream.
      jis0212_ = false;
    }
  }

  SawError ParseByte(uint8_t byte, std::u16string& result) override {
    if (uint8_t lead = std::exchange(lead_, 0x00)) {
      if (lead == 0x8E && byte >= 0xA1 && byte <= 0xDF) {
        result.push_back(static_cast<char16_t>(0xFF61 - 0xA1 + byte));
        return SawError::kNo;
      }
      if (lead == 0x8F && byte >= 0xA1 && byte <= 0xFE) {
        jis0212_ = true;
        lead_ = byte;
        return SawError::kNo;
      }
      if (lead >= 0xA1 && lead <= 0xFE && byte >= 0xA1 && byte <= 0xFE) {
        const auto pointer =
            static_cast<uint16_t>((lead - 0xA1) * 94 + byte - 0xA1);
        if (auto code_point =
                FindCodePoint(std::exchange(jis0212_, false) ? Jis0212Index()
                                                             : Jis0208Index(),
                              pointer)) {
          result.push_back(*code_point);
          return SawError::kNo;
        }
      }
      if (IsAscii(byte)) {
        prepended_byte_ = byte;
      }
      return SawError::kYes;
    }
    if (IsAscii(byte)) {
      result.push_back(byte);
      return SawError::kNo;
    }
    if (byte == 0x8E || byte == 0x8F || (byte >= 0xA1 && byte <= 0xFE)) {
      lead_ = byte;
      return SawError::kNo;
    }
    return SawError::kYes;
  }

 private:
  bool jis0212_ = false;
};

// https://encoding.spec.whatwg.org/#iso-2022-jp-decoder
class Iso2022JpDecoder : public TextCodecCjk::Decoder {
 public:
  std::u16string Decode(base::span<const uint8_t> bytes,
                        bool flush,
                        bool stop_on_error,
                        bool& saw_error) override {
    std::u16string result;
    result.reserve(bytes.size());

    // Reads a byte that an error gave back, if any, and says whether to stop.
    // Blink checks `stop_on_error` before it counts an error of the second
    // byte given back, which this keeps.
    auto parse_prepended = [&](std::optional<uint8_t>& prepended,
                               bool count_only_when_stopping) {
      if (!prepended) {
        return false;
      }
      if (ParseByte(*std::exchange(prepended, std::nullopt), result) !=
              SawError::kYes ||
          (count_only_when_stopping && !stop_on_error)) {
        return false;
      }
      saw_error = true;
      result.push_back(kReplacementCharacter);
      if (stop_on_error) {
        lead_ = 0x00;
        return true;
      }
      return false;
    };

    if (parse_prepended(prepended_byte_, false) ||
        parse_prepended(second_prepended_byte_, true)) {
      return result;
    }
    for (const uint8_t byte : bytes) {
      if (ParseByte(byte, result) == SawError::kYes) {
        saw_error = true;
        result.push_back(kReplacementCharacter);
        if (stop_on_error) {
          lead_ = 0x00;
          return result;
        }
      }
      if (parse_prepended(prepended_byte_, false) ||
          parse_prepended(second_prepended_byte_, true)) {
        return result;
      }
    }

    if (flush) {
      switch (decoder_state_) {
        case State::kAscii:
        case State::kRoman:
        case State::kKatakana:
        case State::kLeadByte:
          break;
        case State::kTrailByte:
          decoder_state_ = State::kLeadByte;
          [[fallthrough]];
        case State::kEscapeStart:
          saw_error = true;
          result.push_back(kReplacementCharacter);
          break;
        case State::kEscape:
          saw_error = true;
          result.push_back(kReplacementCharacter);
          if (lead_) {
            DCHECK(IsAscii(lead_));
            result.push_back(std::exchange(lead_, 0x00));
          }
          break;
      }
      // The stream ends here, so the next one starts afresh.
      lead_ = 0x00;
      prepended_byte_.reset();
      second_prepended_byte_.reset();
      decoder_state_ = State::kAscii;
      decoder_output_state_ = State::kAscii;
      output_ = false;
    }
    return result;
  }

 protected:
  SawError ParseByte(uint8_t byte, std::u16string& result) override {
    switch (decoder_state_) {
      case State::kAscii:
        if (byte == 0x1B) {
          decoder_state_ = State::kEscapeStart;
          break;
        }
        if (byte <= 0x7F && byte != 0x0E && byte != 0x0F && byte != 0x1B) {
          output_ = false;
          result.push_back(byte);
          break;
        }
        output_ = false;
        return SawError::kYes;
      case State::kRoman:
        if (byte == 0x1B) {
          decoder_state_ = State::kEscapeStart;
          break;
        }
        if (byte == 0x5C) {
          output_ = false;
          result.push_back(kYenSign);
          break;
        }
        if (byte == 0x7E) {
          output_ = false;
          result.push_back(kOverline);
          break;
        }
        if (byte <= 0x7F && byte != 0x0E && byte != 0x0F && byte != 0x1B &&
            byte != 0x5C && byte != 0x7E) {
          output_ = false;
          result.push_back(byte);
          break;
        }
        output_ = false;
        return SawError::kYes;
      case State::kKatakana:
        if (byte == 0x1B) {
          decoder_state_ = State::kEscapeStart;
          break;
        }
        if (byte >= 0x21 && byte <= 0x5F) {
          output_ = false;
          result.push_back(static_cast<char16_t>(0xFF61 - 0x21 + byte));
          break;
        }
        output_ = false;
        return SawError::kYes;
      case State::kLeadByte:
        if (byte == 0x1B) {
          decoder_state_ = State::kEscapeStart;
          break;
        }
        if (byte >= 0x21 && byte <= 0x7E) {
          output_ = false;
          lead_ = byte;
          decoder_state_ = State::kTrailByte;
          break;
        }
        output_ = false;
        return SawError::kYes;
      case State::kTrailByte:
        if (byte == 0x1B) {
          decoder_state_ = State::kEscapeStart;
          return SawError::kYes;
        }
        decoder_state_ = State::kLeadByte;
        if (byte >= 0x21 && byte <= 0x7E) {
          const auto pointer =
              static_cast<uint16_t>((lead_ - 0x21) * 94 + byte - 0x21);
          if (auto code_point = FindCodePoint(Jis0208Index(), pointer)) {
            result.push_back(*code_point);
            break;
          }
          return SawError::kYes;
        }
        return SawError::kYes;
      case State::kEscapeStart:
        if (byte == 0x24 || byte == 0x28) {
          lead_ = byte;
          decoder_state_ = State::kEscape;
          break;
        }
        prepended_byte_ = byte;
        output_ = false;
        decoder_state_ = decoder_output_state_;
        return SawError::kYes;
      case State::kEscape: {
        const uint8_t lead = std::exchange(lead_, 0x00);
        std::optional<State> state;
        if (lead == 0x28) {
          if (byte == 0x42) {
            state = State::kAscii;
          } else if (byte == 0x4A) {
            state = State::kRoman;
          } else if (byte == 0x49) {
            state = State::kKatakana;
          }
        } else if (lead == 0x24 && (byte == 0x40 || byte == 0x42)) {
          state = State::kLeadByte;
        }
        if (state) {
          decoder_state_ = *state;
          decoder_output_state_ = *state;
          if (std::exchange(output_, true)) {
            return SawError::kYes;
          }
          break;
        }
        prepended_byte_ = lead;
        second_prepended_byte_ = byte;
        output_ = false;
        decoder_state_ = decoder_output_state_;
        return SawError::kYes;
      }
    }
    return SawError::kNo;
  }

 private:
  enum class State {
    kAscii,
    kRoman,
    kKatakana,
    kLeadByte,
    kTrailByte,
    kEscapeStart,
    kEscape
  };
  State decoder_state_ = State::kAscii;
  State decoder_output_state_ = State::kAscii;
  bool output_ = false;
  std::optional<uint8_t> second_prepended_byte_;
};

// https://encoding.spec.whatwg.org/#shift_jis-decoder
class ShiftJisDecoder : public TextCodecCjk::Decoder {
 protected:
  SawError ParseByte(uint8_t byte, std::u16string& result) override {
    if (uint8_t lead = std::exchange(lead_, 0x00)) {
      const uint8_t offset = byte < 0x7F ? 0x40 : 0x41;
      const uint8_t lead_offset = lead < 0xA0 ? 0x81 : 0xC1;
      if ((byte >= 0x40 && byte <= 0x7E) || (byte >= 0x80 && byte <= 0xFC)) {
        const auto pointer =
            static_cast<uint16_t>((lead - lead_offset) * 188 + byte - offset);
        if (pointer >= 8836 && pointer <= 10715) {
          result.push_back(static_cast<char16_t>(0xE000 - 8836 + pointer));
          return SawError::kNo;
        }
        if (auto code_point = FindCodePoint(Jis0208Index(), pointer)) {
          result.push_back(*code_point);
          return SawError::kNo;
        }
      }
      if (IsAscii(byte)) {
        prepended_byte_ = byte;
      }
      return SawError::kYes;
    }
    if (IsAscii(byte) || byte == 0x80) {
      result.push_back(byte);
      return SawError::kNo;
    }
    if (byte >= 0xA1 && byte <= 0xDF) {
      result.push_back(static_cast<char16_t>(0xFF61 - 0xA1 + byte));
      return SawError::kNo;
    }
    if ((byte >= 0x81 && byte <= 0x9F) || (byte >= 0xE0 && byte <= 0xFC)) {
      lead_ = byte;
      return SawError::kNo;
    }
    return SawError::kYes;
  }
};

// https://encoding.spec.whatwg.org/#euc-kr-decoder
class EucKrDecoder : public TextCodecCjk::Decoder {
 protected:
  SawError ParseByte(uint8_t byte, std::u16string& result) override {
    if (uint8_t lead = std::exchange(lead_, 0x00)) {
      if (byte >= 0x41 && byte <= 0xFE) {
        const auto pointer =
            static_cast<uint16_t>((lead - 0x81) * 190 + byte - 0x41);
        if (auto code_point = FindCodePoint(EucKrIndex(), pointer)) {
          result.push_back(*code_point);
          return SawError::kNo;
        }
      }
      if (IsAscii(byte)) {
        prepended_byte_ = byte;
      }
      return SawError::kYes;
    }
    if (IsAscii(byte)) {
      result.push_back(byte);
      return SawError::kNo;
    }
    if (byte >= 0x81 && byte <= 0xFE) {
      lead_ = byte;
      return SawError::kNo;
    }
    return SawError::kYes;
  }
};

// https://encoding.spec.whatwg.org/#gb18030-decoder, which GBK decodes with
// too, as in Blink.
class Gb18030Decoder : public TextCodecCjk::Decoder {
 public:
  std::u16string Decode(base::span<const uint8_t> bytes,
                        bool flush,
                        bool stop_on_error,
                        bool& saw_error) override {
    saw_error_ = &saw_error;
    std::u16string result =
        TextCodecCjk::Decoder::Decode(bytes, flush, stop_on_error, saw_error);
    saw_error_ = nullptr;
    return result;
  }

 protected:
  SawError ParseByte(uint8_t byte, std::u16string& result) override {
    DCHECK(saw_error_);
    if (third_) {
      if (byte < 0x30 || byte > 0x39) {
        *saw_error_ = true;
        result.push_back(kReplacementCharacter);
        first_ = 0x00;
        const uint8_t second = std::exchange(second_, 0x00);
        const uint8_t third = std::exchange(third_, 0x00);
        if (ParseByte(second, result) == SawError::kYes) {
          *saw_error_ = true;
          result.push_back(kReplacementCharacter);
        }
        if (ParseByte(third, result) == SawError::kYes) {
          *saw_error_ = true;
          result.push_back(kReplacementCharacter);
        }
        return ParseByte(byte, result);
      }
      const uint8_t first = std::exchange(first_, 0x00);
      const uint8_t second = std::exchange(second_, 0x00);
      const uint8_t third = std::exchange(third_, 0x00);
      if (auto code_point = IndexGb18030RangesCodePoint(
              ((first - 0x81) * 10 * 126 * 10) + ((second - 0x30) * 10 * 126) +
              ((third - 0x81) * 10) + byte - 0x30)) {
        AppendCodePoint(result, *code_point);
        return SawError::kNo;
      }
      return SawError::kYes;
    }
    if (second_) {
      if (byte >= 0x81 && byte <= 0xFE) {
        third_ = byte;
        return SawError::kNo;
      }
      *saw_error_ = true;
      result.push_back(kReplacementCharacter);
      first_ = 0x00;
      if (ParseByte(std::exchange(second_, 0x00), result) == SawError::kYes) {
        *saw_error_ = true;
        result.push_back(kReplacementCharacter);
      }
      return ParseByte(byte, result);
    }
    if (first_) {
      if (byte >= 0x30 && byte <= 0x39) {
        second_ = byte;
        return SawError::kNo;
      }
      const uint8_t lead = std::exchange(first_, 0x00);
      const uint8_t offset = byte < 0x7F ? 0x40 : 0x41;
      if ((byte >= 0x40 && byte <= 0x7E) || (byte >= 0x80 && byte <= 0xFE)) {
        const size_t pointer = (lead - 0x81) * 190 + byte - offset;
        if (pointer < Gb18030Table().size()) {
          result.push_back(Gb18030Table()[pointer]);
          return SawError::kNo;
        }
      }
      if (IsAscii(byte)) {
        prepended_byte_ = byte;
      }
      return SawError::kYes;
    }
    if (IsAscii(byte)) {
      result.push_back(byte);
      return SawError::kNo;
    }
    if (byte == 0x80) {
      result.push_back(0x20AC);
      return SawError::kNo;
    }
    if (byte >= 0x81 && byte <= 0xFE) {
      first_ = byte;
      return SawError::kNo;
    }
    return SawError::kYes;
  }

  void Finalize(bool flush, std::u16string& result) override {
    DCHECK(saw_error_);
    if (flush && (first_ || second_ || third_)) {
      first_ = 0x00;
      second_ = 0x00;
      third_ = 0x00;
      *saw_error_ = true;
      result.push_back(kReplacementCharacter);
    }
  }

 private:
  uint8_t first_ = 0x00;
  uint8_t second_ = 0x00;
  uint8_t third_ = 0x00;
  // The caller's `saw_error`, which ParseByte() and Finalize() set, while a
  // call to Decode() lasts.
  raw_ptr<bool> saw_error_ = nullptr;
};

// https://encoding.spec.whatwg.org/#big5-decoder
class Big5Decoder : public TextCodecCjk::Decoder {
 protected:
  SawError ParseByte(uint8_t byte, std::u16string& result) override {
    if (uint8_t lead = std::exchange(lead_, 0x00)) {
      if ((byte >= 0x40 && byte <= 0x7E) || (byte >= 0xA1 && byte <= 0xFE)) {
        const size_t pointer =
            (lead - 0x81) * 157 + (byte - (byte < 0x7F ? 0x40 : 0x62));
        const int32_t code_point = Big5Table()[pointer];
        if (code_point != 0) {
          AppendCodePoint(result, code_point);
          return SawError::kNo;
        }
      }
      if (IsAscii(byte)) {
        prepended_byte_ = byte;
      }
      return SawError::kYes;
    }
    if (IsAscii(byte)) {
      result.push_back(byte);
      return SawError::kNo;
    }
    if (byte >= 0x81 && byte <= 0xFE) {
      lead_ = byte;
      return SawError::kNo;
    }
    return SawError::kYes;
  }
};

}  // namespace

std::u16string TextCodecCjk::Decoder::Decode(base::span<const uint8_t> bytes,
                                             bool flush,
                                             bool stop_on_error,
                                             bool& saw_error) {
  std::u16string result;
  result.reserve(bytes.size());

  // Reads a byte that an error gave back, if any, and says whether to stop.
  auto parse_prepended = [&] {
    if (!prepended_byte_ ||
        ParseByte(*std::exchange(prepended_byte_, std::nullopt), result) !=
            SawError::kYes) {
      return false;
    }
    saw_error = true;
    result.push_back(kReplacementCharacter);
    if (stop_on_error) {
      lead_ = 0x00;
      return true;
    }
    return false;
  };

  if (parse_prepended()) {
    return result;
  }
  for (const uint8_t byte : bytes) {
    if (ParseByte(byte, result) == SawError::kYes) {
      saw_error = true;
      result.push_back(kReplacementCharacter);
      if (stop_on_error) {
        lead_ = 0x00;
        return result;
      }
    }
    if (parse_prepended()) {
      return result;
    }
  }

  if (flush && lead_) {
    lead_ = 0x00;
    saw_error = true;
    result.push_back(kReplacementCharacter);
  }
  Finalize(flush, result);
  return result;
}

// static
bool TextCodecCjk::IsSupported(std::string_view canonical_name) {
  return std::ranges::any_of(kSupportedCanonicalNames, [&](const char* name) {
    return base::EqualsCaseInsensitiveASCII(canonical_name, name);
  });
}

// static
void TextCodecCjk::RegisterEncodingNames(EncodingNameRegistrar registrar) {
  // https://encoding.spec.whatwg.org/#names-and-labels: each list's first is
  // the canonical name.
  auto register_aliases = [&](std::initializer_list<const char*> list) {
    for (const char* alias : list) {
      registrar(alias, *list.begin());
    }
  };
  register_aliases({kCanonicalNameEucJp, "cseucpkdfmtjapanese", "x-euc-jp"});
  register_aliases({kCanonicalNameShiftJis, "csshiftjis", "ms932", "ms_kanji",
                    "shift-jis", "sjis", "windows-31j", "x-sjis"});
  register_aliases({kCanonicalNameEucKr, "cseuckr", "csksc56011987",
                    "iso-ir-149", "korean", "ks_c_5601-1987", "ks_c_5601-1989",
                    "ksc5601", "ksc_5601", "windows-949"});
  register_aliases({kCanonicalNameIso2022Jp, "csiso2022jp"});
  register_aliases({kCanonicalNameGbk, "chinese", "csgb2312", "csiso58gb231280",
                    "gb2312", "gb_2312", "gb_2312-80", "iso-ir-58", "x-gbk"});
  register_aliases({kCanonicalNameGb18030});
  register_aliases({kCanonicalNameBig5, "cn-big5", "csbig5", "x-x-big5",
                    kCanonicalNameBig5Hkscs});
}

// static
void TextCodecCjk::RegisterCodecs(TextCodecRegistrar registrar) {
  for (const char* name : kSupportedCanonicalNames) {
    registrar(name, &Create);
  }
}

// static
std::unique_ptr<TextCodec> TextCodecCjk::Create(
    std::string_view canonical_name) {
  if (canonical_name == kCanonicalNameEucJp) {
    return std::make_unique<TextCodecCjk>(Encoding::kEucJp);
  }
  if (canonical_name == kCanonicalNameShiftJis) {
    return std::make_unique<TextCodecCjk>(Encoding::kShiftJis);
  }
  if (canonical_name == kCanonicalNameEucKr) {
    return std::make_unique<TextCodecCjk>(Encoding::kEucKr);
  }
  if (canonical_name == kCanonicalNameIso2022Jp) {
    return std::make_unique<TextCodecCjk>(Encoding::kIso2022Jp);
  }
  if (canonical_name == kCanonicalNameGbk) {
    return std::make_unique<TextCodecCjk>(Encoding::kGbk);
  }
  if (canonical_name == kCanonicalNameGb18030) {
    return std::make_unique<TextCodecCjk>(Encoding::kGb18030);
  }
  if (canonical_name == kCanonicalNameBig5 ||
      canonical_name == kCanonicalNameBig5Hkscs) {
    return std::make_unique<TextCodecCjk>(Encoding::kBig5);
  }
  NOTREACHED();
}

TextCodecCjk::TextCodecCjk(Encoding encoding) : encoding_(encoding) {}

TextCodecCjk::~TextCodecCjk() = default;

std::u16string TextCodecCjk::Decode(base::span<const uint8_t> data,
                                    FlushBehavior flush_behavior,
                                    bool stop_on_error,
                                    bool& saw_error) {
  const bool flush = flush_behavior != FlushBehavior::kDoNotFlush;
  if (!decoder_) {
    switch (encoding_) {
      case Encoding::kEucJp:
        decoder_ = std::make_unique<EucJpDecoder>();
        break;
      case Encoding::kShiftJis:
        decoder_ = std::make_unique<ShiftJisDecoder>();
        break;
      case Encoding::kIso2022Jp:
        decoder_ = std::make_unique<Iso2022JpDecoder>();
        break;
      case Encoding::kEucKr:
        decoder_ = std::make_unique<EucKrDecoder>();
        break;
      case Encoding::kGbk:
      case Encoding::kGb18030:
        decoder_ = std::make_unique<Gb18030Decoder>();
        break;
      case Encoding::kBig5:
        decoder_ = std::make_unique<Big5Decoder>();
        break;
    }
  }
  return decoder_->Decode(data, flush, stop_on_error, saw_error);
}

std::string TextCodecCjk::Encode(std::u16string_view text,
                                 UnencodableHandling handling) {
  switch (encoding_) {
    case Encoding::kEucJp:
      return EncodeEucJp(text, handling);
    case Encoding::kShiftJis:
      return EncodeShiftJis(text, handling);
    case Encoding::kIso2022Jp:
      return Iso2022JpEncoder::Encode(text, handling);
    case Encoding::kEucKr:
      return EncodeEucKr(text, handling);
    case Encoding::kGbk:
      return EncodeGbShared(text, handling, IsGbk::kYes);
    case Encoding::kGb18030:
      return EncodeGbShared(text, handling, IsGbk::kNo);
    case Encoding::kBig5:
      return EncodeBig5(text, handling);
  }
  NOTREACHED();
}

}  // namespace mitto
