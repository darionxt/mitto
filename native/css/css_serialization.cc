#include "mitto/css/css_serialization.h"

#include <cmath>

#include "base/numerics/safe_conversions.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/third_party/icu/icu_utf.h"

namespace mitto {

namespace {

void AppendCodePoint(char32_t c, std::u16string& output) {
  if (c <= 0xFFFF) {
    output.push_back(static_cast<char16_t>(c));
  } else {
    output.push_back(static_cast<char16_t>(0xD7C0 + (c >> 10)));
    output.push_back(static_cast<char16_t>(0xDC00 | (c & 0x3FF)));
  }
}

void SerializeCharacter(char32_t c, std::u16string& append_to) {
  append_to.push_back(u'\\');
  AppendCodePoint(c, append_to);
}

void SerializeCharacterAsCodePoint(char32_t c, std::u16string& append_to) {
  append_to +=
      base::ASCIIToUTF16(base::StringPrintf("\\%x ", static_cast<unsigned>(c)));
}

}  // namespace

void SerializeIdentifier(std::u16string_view identifier,
                         std::u16string& append_to) {
  bool is_first = true;
  bool is_second = false;
  bool is_first_char_hyphen = false;
  size_t index = 0;
  while (index < identifier.size()) {
    char32_t c = identifier[index];
    size_t length = 1;
    if (CBU16_IS_LEAD(c) && index + 1 < identifier.size() &&
        CBU16_IS_TRAIL(identifier[index + 1])) {
      c = CBU16_GET_SUPPLEMENTARY(c, identifier[index + 1]);
      length = 2;
    }
    index += length;

    if (c == 0) {
      append_to.push_back(0xFFFD);
    } else if (c <= 0x1F || c == 0x7F ||
               (0x30 <= c && c <= 0x39 &&
                (is_first || (is_second && is_first_char_hyphen)))) {
      SerializeCharacterAsCodePoint(c, append_to);
    } else if (c == 0x2D && is_first && index == identifier.size()) {
      SerializeCharacter(c, append_to);
    } else if (0x80 <= c || c == 0x2D || c == 0x5F ||
               (0x30 <= c && c <= 0x39) || (0x41 <= c && c <= 0x5A) ||
               (0x61 <= c && c <= 0x7A)) {
      AppendCodePoint(c, append_to);
    } else {
      SerializeCharacter(c, append_to);
    }

    if (is_first) {
      is_first = false;
      is_second = true;
      is_first_char_hyphen = (c == 0x2D);
    } else if (is_second) {
      is_second = false;
    }
  }
}

// As Blink's CSSNumericLiteralValue::CustomCSSText().
std::u16string NumericLiteralCssText(double value, CssUnit unit) {
  const std::string_view unit_name = CssUnitName(unit);
  if (unit == CssUnit::kInteger) {
    return base::NumberToString16(base::saturated_cast<int>(value));
  }
  // The integers that six digits write without an exponent.
  constexpr int kMinInteger = -999999;
  constexpr int kMaxInteger = 999999;
  if (value < kMinInteger || value > kMaxInteger ||
      std::trunc(value) != value) {
    if (!std::isfinite(value)) {
      std::string text = std::isnan(value) ? "NaN"
                         : value > 0       ? "infinity"
                                           : "-infinity";
      if (!unit_name.empty()) {
        text += " * 1";
        text += unit_name;
      }
      return base::ASCIIToUTF16(text);
    }
    // Blink rounds half to even here, as printf does.
    return base::ASCIIToUTF16(base::StringPrintf("%.6g", value)) +
           base::ASCIIToUTF16(unit_name);
  }
  return base::NumberToString16(static_cast<int>(value)) +
         base::ASCIIToUTF16(unit_name);
}

}  // namespace mitto
