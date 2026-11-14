#include "mitto/dom/svg_animated_values.h"

#include <cmath>
#include <limits>
#include <string>
#include <utility>

#include "base/numerics/safe_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/third_party/double_conversion/double-conversion/double-conversion.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_svg_animated_boolean.h"
#include "mitto/bindings/v8_svg_animated_enumeration.h"
#include "mitto/bindings/v8_svg_animated_integer.h"
#include "mitto/bindings/v8_svg_animated_number.h"
#include "mitto/bindings/v8_svg_animated_string.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/svg_element.h"
#include "third_party/icu/source/common/unicode/uchar.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

namespace {

bool IsHTMLSpace(char16_t c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

bool IsDigit(char16_t c) {
  return c >= '0' && c <= '9';
}

void SkipSpaces(std::u16string_view& value) {
  while (!value.empty() && IsHTMLSpace(value.front())) {
    value.remove_prefix(1);
  }
}

// SkipOptionalSVGSpacesOrDelimiter() with a comma.
void SkipSpacesOrComma(std::u16string_view& value) {
  if (!value.empty() && !IsHTMLSpace(value.front()) && value.front() != ',') {
    return;
  }
  SkipSpaces(value);
  if (!value.empty() && value.front() == ',') {
    value.remove_prefix(1);
    SkipSpaces(value);
  }
}

bool IsValidRange(float value) {
  constexpr float kMax = std::numeric_limits<float>::max();
  return value >= -kMax && value <= kMax;
}

enum WhitespaceMode {
  kAllowLeadingWhitespace = 1,
  kAllowTrailingWhitespace = 2,
  kAllowLeadingAndTrailingWhitespace = 3,
};

// GenericParseNumber() for a float: a number from the start of `value`, which
// it then starts after, in Blink's arithmetic of floats.
bool ParseNumber(std::u16string_view& value,
                 float& number,
                 int mode = kAllowLeadingAndTrailingWhitespace) {
  if (mode & kAllowLeadingWhitespace) {
    SkipSpaces(value);
  }
  std::u16string_view rest = value;
  int sign = 1;
  if (!rest.empty() && rest.front() == '+') {
    rest.remove_prefix(1);
  } else if (!rest.empty() && rest.front() == '-') {
    rest.remove_prefix(1);
    sign = -1;
  }
  if (rest.empty() || (!IsDigit(rest.front()) && rest.front() != '.')) {
    return false;
  }
  size_t digits_count = 0;
  while (digits_count < rest.size() && IsDigit(rest[digits_count])) {
    ++digits_count;
  }
  float integer = 0;
  if (digits_count != 0) {
    float multiplier = 1;
    for (size_t index = digits_count; index > 0; --index) {
      integer += multiplier * static_cast<float>(rest[index - 1] - '0');
      multiplier *= 10;
    }
    if (!IsValidRange(integer)) {
      return false;
    }
    rest.remove_prefix(digits_count);
  }
  float decimal = 0;
  if (!rest.empty() && rest.front() == '.') {
    rest.remove_prefix(1);
    // A digit must follow the point.
    if (rest.empty() || !IsDigit(rest.front())) {
      return false;
    }
    float fraction = 1;
    size_t decimal_count = 0;
    while (decimal_count < rest.size() && IsDigit(rest[decimal_count])) {
      fraction *= static_cast<float>(0.1);
      decimal += (rest[decimal_count] - '0') * fraction;
      ++decimal_count;
    }
    rest.remove_prefix(decimal_count);
  }
  number = integer + decimal;
  number *= sign;
  // An exponent, but not the start of a unit such as ex or em.
  if (rest.size() > 1 && (rest[0] == 'e' || rest[0] == 'E') && rest[1] != 'x' &&
      rest[1] != 'm') {
    rest.remove_prefix(1);
    bool exponent_is_negative = false;
    if (rest.front() == '+') {
      rest.remove_prefix(1);
    } else if (rest.front() == '-') {
      rest.remove_prefix(1);
      exponent_is_negative = true;
    }
    if (rest.empty() || !IsDigit(rest.front())) {
      return false;
    }
    float exponent = 0;
    size_t exponent_count = 0;
    while (exponent_count < rest.size() && IsDigit(rest[exponent_count])) {
      exponent *= static_cast<float>(10);
      exponent += rest[exponent_count] - '0';
      ++exponent_count;
    }
    if (exponent_is_negative) {
      exponent = -exponent;
    }
    if (exponent > std::numeric_limits<float>::max_exponent10) {
      return false;
    }
    if (exponent) {
      number *= static_cast<float>(std::pow(10.0, exponent));
    }
    rest.remove_prefix(exponent_count);
  }
  if (!IsValidRange(number)) {
    return false;
  }
  value = rest;
  if (mode & kAllowTrailingWhitespace) {
    SkipSpacesOrComma(value);
  }
  return true;
}

// SVGNumberAcceptPercentage's parse: a hundredth of a percentage.
std::optional<float> ParseNumberOrPercentage(std::u16string_view value) {
  float number = 0;
  if (!ParseNumber(value, number, kAllowLeadingWhitespace)) {
    return std::nullopt;
  }
  if (!value.empty() && value.front() == '%') {
    value.remove_prefix(1);
    number /= 100;
  }
  SkipSpaces(value);
  if (!value.empty()) {
    return std::nullopt;
  }
  return number;
}

// ParseNumberOptionalNumber(): one number, which is both, or two.
std::optional<std::pair<float, float>> ParseNumberOptionalNumber(
    std::u16string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }
  float x = 0;
  float y = 0;
  if (!ParseNumber(value, x)) {
    return std::nullopt;
  }
  if (value.empty()) {
    y = x;
  } else if (!ParseNumber(value, y)) {
    return std::nullopt;
  }
  if (!value.empty()) {
    return std::nullopt;
  }
  return std::pair(x, y);
}

// IsSpaceOrNewline(), which StringToIntStrict() skips around the number.
bool IsSpaceOrNewline(char16_t c) {
  if (c <= 0x7f) {
    return c == ' ' || (c >= '\t' && c <= '\r');
  }
  return u_charDirection(c) == U_WHITE_SPACE_NEUTRAL;
}

// SVGInteger's parse: StringToIntStrict() of the value without its HTML
// spaces, which takes a sign but nothing past the digits, and no number that
// an int cannot hold.
std::optional<int32_t> ParseInteger(std::u16string_view value) {
  SkipSpaces(value);
  while (!value.empty() && IsHTMLSpace(value.back())) {
    value.remove_suffix(1);
  }
  while (!value.empty() && IsSpaceOrNewline(value.front())) {
    value.remove_prefix(1);
  }
  while (!value.empty() && IsSpaceOrNewline(value.back())) {
    value.remove_suffix(1);
  }
  bool negative = false;
  if (!value.empty() && (value.front() == '+' || value.front() == '-')) {
    negative = value.front() == '-';
    value.remove_prefix(1);
  }
  if (value.empty()) {
    return std::nullopt;
  }
  int64_t number = 0;
  for (const char16_t c : value) {
    if (!IsDigit(c)) {
      return std::nullopt;
    }
    number = number * 10 + (c - '0');
    if (number > int64_t{std::numeric_limits<int32_t>::max()} + 1) {
      return std::nullopt;
    }
  }
  if (negative) {
    number = -number;
  }
  if (!base::IsValueInRangeForNumericType<int32_t>(number)) {
    return std::nullopt;
  }
  return static_cast<int32_t>(number);
}

template <typename T>
std::u16string PairToString(T first, T second, std::u16string (*format)(T)) {
  if (first == second) {
    return format(first);
  }
  return base::StrCat({format(first), u" ", format(second)});
}

std::u16string IntegerToString(int32_t value) {
  return base::NumberToString16(value);
}

}  // namespace

std::optional<float> ParseSVGNumber(std::u16string_view value) {
  float number = 0;
  if (!ParseNumber(value, number) || !value.empty()) {
    return std::nullopt;
  }
  return number;
}

// DoubleToStringConverter::ToStringWithFixedPrecision() of WTF's dtoa.cc.
std::u16string SVGNumberToString(float value) {
  char buffer[128];
  double_conversion::StringBuilder builder(buffer, sizeof(buffer));
  double_conversion::DoubleToStringConverter::EcmaScriptConverter().ToPrecision(
      static_cast<double>(value), 6, &builder);
  std::string text(builder.Finalize());
  // Blink keeps the trailing zeros of a number with an exponent.
  if (text.find('e') == std::string::npos &&
      text.find('.') != std::string::npos) {
    while (text.back() == '0') {
      text.pop_back();
    }
    if (text.back() == '.') {
      text.pop_back();
    }
  }
  return std::u16string(text.begin(), text.end());
}

SVGAnimatedPropertyBase::SVGAnimatedPropertyBase(SVGElement& element,
                                                 std::u16string_view attribute,
                                                 SVGPairHalf half)
    : element_(&element), attribute_(attribute), half_(half) {}

SVGAnimatedPropertyBase::~SVGAnimatedPropertyBase() = default;

void SVGAnimatedPropertyBase::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(element_);
}

bool SVGAnimatedPropertyBase::ReadsAttribute(const QualifiedName& name) const {
  return !name.namespace_uri && name.local_name == attribute_;
}

const std::u16string* SVGAnimatedPropertyBase::AttributeValue() const {
  return element_->FindAttribute(
      QualifiedName{.local_name = std::u16string(attribute_)});
}

void SVGAnimatedPropertyBase::SynchronizeAttribute(
    const std::u16string& value) {
  element_->SynchronizeAnimatedAttribute(
      QualifiedName{.local_name = std::u16string(attribute_)}, value);
}

void SVGAnimatedPropertyBase::EnsureFresh() {
  if (!stale_) {
    return;
  }
  stale_ = false;
  Parse(AttributeValue());
}

// static
SVGAnimatedString* SVGAnimatedString::Create(SVGElement& element,
                                             std::u16string_view attribute) {
  return bindings::MakeGarbageCollected<SVGAnimatedString>(element, attribute);
}

SVGAnimatedString::SVGAnimatedString(SVGElement& element,
                                     std::u16string_view attribute)
    : SVGAnimatedPropertyBase(element, attribute, SVGPairHalf::kNone) {}

SVGAnimatedString::~SVGAnimatedString() = default;

const bindings::WrapperTypeInfo* SVGAnimatedString::GetWrapperTypeInfo() const {
  return &bindings::V8SVGAnimatedString::kWrapperTypeInfo;
}

QualifiedName SVGAnimatedString::BackingName() const {
  const QualifiedName name{.local_name = std::u16string(AttributeName())};
  if (AttributeName() != u"href" || ContextElement().FindAttribute(name)) {
    return name;
  }
  const QualifiedName xlink_name{.local_name = u"href",
                                 .namespace_uri = html_names::kXlinkNamespace};
  return ContextElement().FindAttribute(xlink_name) ? xlink_name : name;
}

bool SVGAnimatedString::ReadsAttribute(const QualifiedName& name) const {
  return SVGAnimatedPropertyBase::ReadsAttribute(name) ||
         (AttributeName() == u"href" &&
          name.namespace_uri == html_names::kXlinkNamespace &&
          name.local_name == u"href");
}

std::u16string SVGAnimatedString::BaseVal() const {
  const std::u16string* const value =
      ContextElement().FindAttribute(BackingName());
  return value ? *value : std::u16string();
}

void SVGAnimatedString::SetBaseVal(const std::u16string& value,
                                   bindings::ExceptionState& exception_state) {
  ContextElement().SynchronizeAnimatedAttribute(BackingName(), value);
}

// static
SVGAnimatedBoolean* SVGAnimatedBoolean::Create(SVGElement& element,
                                               std::u16string_view attribute) {
  return bindings::MakeGarbageCollected<SVGAnimatedBoolean>(element, attribute);
}

SVGAnimatedBoolean::SVGAnimatedBoolean(SVGElement& element,
                                       std::u16string_view attribute)
    : SVGAnimatedPropertyBase(element, attribute, SVGPairHalf::kNone) {}

SVGAnimatedBoolean::~SVGAnimatedBoolean() = default;

const bindings::WrapperTypeInfo* SVGAnimatedBoolean::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGAnimatedBoolean::kWrapperTypeInfo;
}

bool SVGAnimatedBoolean::BaseVal() {
  EnsureFresh();
  return value_;
}

void SVGAnimatedBoolean::SetBaseVal(bool value,
                                    bindings::ExceptionState& exception_state) {
  EnsureFresh();
  value_ = value;
  SynchronizeAttribute(value ? u"true" : u"false");
}

void SVGAnimatedBoolean::Parse(const std::u16string* value) {
  value_ = value && *value == u"true";
}

// static
SVGAnimatedEnumeration* SVGAnimatedEnumeration::Create(
    SVGElement& element,
    std::u16string_view attribute,
    const SVGEnumerationMap& map,
    uint16_t initial_value) {
  return bindings::MakeGarbageCollected<SVGAnimatedEnumeration>(
      element, attribute, map, initial_value);
}

SVGAnimatedEnumeration::SVGAnimatedEnumeration(SVGElement& element,
                                               std::u16string_view attribute,
                                               const SVGEnumerationMap& map,
                                               uint16_t initial_value)
    : SVGAnimatedPropertyBase(element, attribute, SVGPairHalf::kNone),
      map_(map),
      initial_value_(initial_value) {}

SVGAnimatedEnumeration::~SVGAnimatedEnumeration() = default;

const bindings::WrapperTypeInfo* SVGAnimatedEnumeration::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGAnimatedEnumeration::kWrapperTypeInfo;
}

uint16_t SVGAnimatedEnumeration::BaseVal() {
  EnsureFresh();
  return value_ <= map_->max_exposed_value ? value_ : 0;
}

void SVGAnimatedEnumeration::SetBaseVal(
    uint16_t value,
    bindings::ExceptionState& exception_state) {
  if (!value) {
    exception_state.ThrowTypeError(
        "The enumeration value provided is 0, which is not settable.");
    return;
  }
  if (value > map_->max_exposed_value) {
    exception_state.ThrowTypeError(base::StrCat(
        {"The enumeration value provided (", base::NumberToString(value),
         ") is larger than the largest allowed value (",
         base::NumberToString(map_->max_exposed_value), ")."}));
    return;
  }
  EnsureFresh();
  value_ = value;
  SynchronizeAttribute(std::u16string(map_->keywords[value - 1]));
}

void SVGAnimatedEnumeration::Parse(const std::u16string* value) {
  value_ = initial_value_;
  if (!value) {
    return;
  }
  for (size_t i = 0; i < map_->keywords.size(); ++i) {
    if (*value == map_->keywords[i]) {
      value_ = static_cast<uint16_t>(i + 1);
      return;
    }
  }
}

// static
SVGAnimatedInteger* SVGAnimatedInteger::Create(SVGElement& element,
                                               std::u16string_view attribute,
                                               int32_t initial_value) {
  return bindings::MakeGarbageCollected<SVGAnimatedInteger>(
      element, attribute, SVGPairHalf::kNone, initial_value);
}

// static
SVGAnimatedInteger* SVGAnimatedInteger::CreateOrderHalf(SVGElement& element,
                                                        SVGPairHalf half,
                                                        int32_t initial_value) {
  return bindings::MakeGarbageCollected<SVGAnimatedInteger>(
      element, u"order", half, initial_value);
}

SVGAnimatedInteger::SVGAnimatedInteger(SVGElement& element,
                                       std::u16string_view attribute,
                                       SVGPairHalf half,
                                       int32_t initial_value)
    : SVGAnimatedPropertyBase(element, attribute, half),
      initial_value_(initial_value) {}

SVGAnimatedInteger::~SVGAnimatedInteger() = default;

const bindings::WrapperTypeInfo* SVGAnimatedInteger::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGAnimatedInteger::kWrapperTypeInfo;
}

void SVGAnimatedInteger::Trace(cppgc::Visitor* visitor) const {
  SVGAnimatedPropertyBase::Trace(visitor);
  visitor->Trace(other_half_);
}

int32_t SVGAnimatedInteger::BaseVal() {
  EnsureFresh();
  return value_;
}

void SVGAnimatedInteger::SetBaseVal(int32_t value,
                                    bindings::ExceptionState& exception_state) {
  EnsureFresh();
  value_ = value;
  if (Half() == SVGPairHalf::kNone) {
    SynchronizeAttribute(IntegerToString(value));
    return;
  }
  const int32_t other = other_half_->BaseVal();
  SynchronizeAttribute(Half() == SVGPairHalf::kFirst
                           ? PairToString(value, other, &IntegerToString)
                           : PairToString(other, value, &IntegerToString));
}

void SVGAnimatedInteger::Parse(const std::u16string* value) {
  value_ = initial_value_;
  if (!value) {
    return;
  }
  if (Half() == SVGPairHalf::kNone) {
    value_ = ParseInteger(*value).value_or(initial_value_);
    return;
  }
  // SVGIntegerOptionalInteger reads numbers and keeps their integer parts,
  // and SVGAnimatedOrder wants both positive.
  const std::optional<std::pair<float, float>> pair =
      ParseNumberOptionalNumber(*value);
  if (!pair) {
    return;
  }
  const int32_t first = base::saturated_cast<int32_t>(pair->first);
  const int32_t second = base::saturated_cast<int32_t>(pair->second);
  if (first < 1 || second < 1) {
    return;
  }
  value_ = Half() == SVGPairHalf::kFirst ? first : second;
}

// static
SVGAnimatedNumber* SVGAnimatedNumber::Create(SVGElement& element,
                                             std::u16string_view attribute,
                                             float initial_value,
                                             Syntax syntax) {
  return bindings::MakeGarbageCollected<SVGAnimatedNumber>(
      element, attribute, SVGPairHalf::kNone, initial_value, syntax);
}

// static
SVGAnimatedNumber* SVGAnimatedNumber::CreateHalf(SVGElement& element,
                                                 std::u16string_view attribute,
                                                 SVGPairHalf half,
                                                 float initial_value) {
  return bindings::MakeGarbageCollected<SVGAnimatedNumber>(
      element, attribute, half, initial_value, Syntax::kNumberOptionalNumber);
}

SVGAnimatedNumber::SVGAnimatedNumber(SVGElement& element,
                                     std::u16string_view attribute,
                                     SVGPairHalf half,
                                     float initial_value,
                                     Syntax syntax)
    : SVGAnimatedPropertyBase(element, attribute, half),
      initial_value_(initial_value),
      syntax_(syntax) {}

SVGAnimatedNumber::~SVGAnimatedNumber() = default;

const bindings::WrapperTypeInfo* SVGAnimatedNumber::GetWrapperTypeInfo() const {
  return &bindings::V8SVGAnimatedNumber::kWrapperTypeInfo;
}

void SVGAnimatedNumber::Trace(cppgc::Visitor* visitor) const {
  SVGAnimatedPropertyBase::Trace(visitor);
  visitor->Trace(other_half_);
}

float SVGAnimatedNumber::BaseVal() {
  EnsureFresh();
  return value_;
}

void SVGAnimatedNumber::SetBaseVal(float value,
                                   bindings::ExceptionState& exception_state) {
  SetBaseValue(value);
}

void SVGAnimatedNumber::SetBaseValue(float value) {
  EnsureFresh();
  value_ = value;
  if (Half() == SVGPairHalf::kNone) {
    SynchronizeAttribute(SVGNumberToString(value));
    return;
  }
  const float other = other_half_->BaseVal();
  SynchronizeAttribute(Half() == SVGPairHalf::kFirst
                           ? PairToString(value, other, &SVGNumberToString)
                           : PairToString(other, value, &SVGNumberToString));
}

void SVGAnimatedNumber::Parse(const std::u16string* value) {
  value_ = initial_value_;
  if (!value) {
    return;
  }
  switch (syntax_) {
    case Syntax::kNumber:
      value_ = ParseSVGNumber(*value).value_or(initial_value_);
      return;
    case Syntax::kNumberOrPercentage:
      value_ = ParseNumberOrPercentage(*value).value_or(initial_value_);
      return;
    case Syntax::kNumberOptionalNumber:
      if (const std::optional<std::pair<float, float>> pair =
              ParseNumberOptionalNumber(*value)) {
        value_ = Half() == SVGPairHalf::kFirst ? pair->first : pair->second;
      }
      return;
  }
}

}  // namespace mitto
