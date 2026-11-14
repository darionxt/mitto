#include "mitto/dom/html_parser_idioms.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include "base/third_party/double_conversion/double-conversion/double-conversion.h"

namespace mitto {

namespace {

bool IsAsciiDigit(char16_t c) {
  return c >= '0' && c <= '9';
}

std::u16string_view SkipHTMLSpaces(std::u16string_view value) {
  while (!value.empty() && IsHTMLSpace(value.front())) {
    value.remove_prefix(1);
  }
  return value;
}

}  // namespace

bool IsHTMLSpace(char16_t c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

std::u16string_view StripHTMLSpaces(std::u16string_view value) {
  value = SkipHTMLSpaces(value);
  while (!value.empty() && IsHTMLSpace(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

std::optional<int32_t> ParseHTMLInteger(std::u16string_view value) {
  value = SkipHTMLSpaces(value);
  bool negative = false;
  if (!value.empty() && (value.front() == '-' || value.front() == '+')) {
    negative = value.front() == '-';
    value.remove_prefix(1);
  }
  if (value.empty() || !IsAsciiDigit(value.front())) {
    return std::nullopt;
  }
  constexpr int64_t kLimit = int64_t{std::numeric_limits<int32_t>::max()} + 1;
  int64_t result = 0;
  for (const char16_t c : value) {
    if (!IsAsciiDigit(c)) {
      break;
    }
    result = result * 10 + (c - '0');
    if (result > kLimit) {
      return std::nullopt;
    }
  }
  if (negative) {
    result = -result;
  }
  if (result > std::numeric_limits<int32_t>::max()) {
    return std::nullopt;
  }
  return static_cast<int32_t>(result);
}

namespace {

enum class NumberParsingResult { kError, kOverflowMax, kSuccess };

// Blink's ParseHTMLNonNegativeIntegerInternal().
NumberParsingResult ParseNonNegative(std::u16string_view value,
                                     uint32_t& result_value) {
  value = SkipHTMLSpaces(value);
  bool negative = false;
  if (!value.empty() && (value.front() == '-' || value.front() == '+')) {
    negative = value.front() == '-';
    value.remove_prefix(1);
  }
  if (value.empty() || !IsAsciiDigit(value.front())) {
    return NumberParsingResult::kError;
  }
  uint64_t result = 0;
  for (const char16_t c : value) {
    if (!IsAsciiDigit(c)) {
      break;
    }
    // A minus sign takes only zeros, as WTF's AcceptMinusZeroForUnsigned.
    if (negative && c != '0') {
      return NumberParsingResult::kError;
    }
    result = result * 10 + (c - '0');
    if (result > std::numeric_limits<uint32_t>::max()) {
      return NumberParsingResult::kOverflowMax;
    }
  }
  result_value = static_cast<uint32_t>(result);
  return NumberParsingResult::kSuccess;
}

}  // namespace

std::optional<uint32_t> ParseHTMLNonNegativeInteger(std::u16string_view value) {
  uint32_t result = 0;
  if (ParseNonNegative(value, result) != NumberParsingResult::kSuccess) {
    return std::nullopt;
  }
  return result;
}

std::optional<uint32_t> ParseHTMLClampedNonNegativeInteger(
    std::u16string_view value,
    uint32_t min,
    uint32_t max) {
  uint32_t result = 0;
  switch (ParseNonNegative(value, result)) {
    case NumberParsingResult::kError:
      return std::nullopt;
    case NumberParsingResult::kOverflowMax:
      return max;
    case NumberParsingResult::kSuccess:
      return std::clamp(result, min, max);
  }
}

std::optional<double> ParseHTMLFloatingPointNumber(std::u16string_view value) {
  value = SkipHTMLSpaces(value);
  if (value.empty() || !(IsAsciiDigit(value.front()) || value.front() == '+' ||
                         value.front() == '-' || value.front() == '.')) {
    return std::nullopt;
  }
  // WTF's ParseDouble() reads ASCII alone: the number ends where it does.
  std::string ascii;
  for (const char16_t c : value) {
    if (c > 0x7f) {
      break;
    }
    ascii.push_back(static_cast<char>(c));
  }
  const double_conversion::StringToDoubleConverter converter(
      double_conversion::StringToDoubleConverter::ALLOW_TRAILING_JUNK, 0.0,
      std::numeric_limits<double>::quiet_NaN(), nullptr, nullptr);
  int processed = 0;
  const double number = converter.StringToDouble(
      ascii.data(), static_cast<int>(ascii.size()), &processed);
  if (!processed || !std::isfinite(number)) {
    return std::nullopt;
  }
  return number == 0 ? 0 : number;
}

std::u16string SerializeForNumberType(double value) {
  char buffer[128];
  double_conversion::StringBuilder builder(buffer, sizeof(buffer));
  double_conversion::DoubleToStringConverter::EcmaScriptConverter().ToShortest(
      value == 0 ? 0 : value, &builder);
  const std::string text(builder.Finalize());
  return std::u16string(text.begin(), text.end());
}

}  // namespace mitto
