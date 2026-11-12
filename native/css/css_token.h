// A token of CSS Syntax's tokenizer, as Blink's CSSParserToken holds it, and
// the units of a dimension, as Blink's CSSPrimitiveValue::UnitType names them:
// https://drafts.csswg.org/css-syntax/#tokenization
//
// Upstream: third_party/blink/renderer/core/css/parser/css_parser_token.h
// Upstream: third_party/blink/renderer/core/css/css_primitive_value.h
// Upstream: third_party/blink/renderer/core/css/css_primitive_value_units.json5

#ifndef MITTO_CSS_CSS_TOKEN_H_
#define MITTO_CSS_CSS_TOKEN_H_

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <string_view>

namespace mitto {

enum class CssTokenType : uint8_t {
  kIdent,
  kFunction,
  kAtKeyword,
  kHash,
  kUrl,
  kBadUrl,
  kDelimiter,
  kNumber,
  kPercentage,
  kDimension,
  kIncludeMatch,
  kDashMatch,
  kPrefixMatch,
  kSuffixMatch,
  kSubstringMatch,
  kColumn,
  kUnicodeRange,
  kWhitespace,
  kCdo,
  kCdc,
  kColon,
  kSemicolon,
  kComma,
  kLeftParenthesis,
  kRightParenthesis,
  kLeftBracket,
  kRightBracket,
  kLeftBrace,
  kRightBrace,
  kString,
  kBadString,
  kEndOfFile,
};

// The units that Blink knows, in the order of CSSPrimitiveValue::UnitType for
// those a media query can hold.
enum class CssUnit : uint8_t {
  kUnknown,
  kNumber,
  kPercentage,
  kEms,
  kExs,
  kPixels,
  kCentimeters,
  kMillimeters,
  kInches,
  kPoints,
  kPicas,
  kQuarterMillimeters,
  kViewportWidth,
  kViewportHeight,
  kViewportInlineSize,
  kViewportBlockSize,
  kViewportMin,
  kViewportMax,
  kSmallViewportWidth,
  kSmallViewportHeight,
  kSmallViewportInlineSize,
  kSmallViewportBlockSize,
  kSmallViewportMin,
  kSmallViewportMax,
  kLargeViewportWidth,
  kLargeViewportHeight,
  kLargeViewportInlineSize,
  kLargeViewportBlockSize,
  kLargeViewportMin,
  kLargeViewportMax,
  kDynamicViewportWidth,
  kDynamicViewportHeight,
  kDynamicViewportInlineSize,
  kDynamicViewportBlockSize,
  kDynamicViewportMin,
  kDynamicViewportMax,
  kContainerWidth,
  kContainerHeight,
  kContainerInlineSize,
  kContainerBlockSize,
  kContainerMin,
  kContainerMax,
  kRems,
  kChs,
  kDegrees,
  kRadians,
  kGradians,
  kTurns,
  kMilliseconds,
  kSeconds,
  kHertz,
  kKilohertz,
  kDotsPerPixel,
  kX,
  kDotsPerInch,
  kDotsPerCentimeter,
  kFlex,
  kRexs,
  kRchs,
  kRics,
  kIcs,
  kLhs,
  kRlhs,
  kCaps,
  kRcaps,
  kQuirkyEms,
  // An integer, which a parser makes of a number token where it takes one.
  kInteger,
};

// What a unit measures, as CSSPrimitiveValue::UnitCategory has it. A length
// relative to a font or the viewport is kOther, since nothing converts it to
// pixels without them.
enum class CssUnitCategory : uint8_t {
  kNumber,
  kPercent,
  kLength,
  kAngle,
  kTime,
  kFrequency,
  kResolution,
  kOther,
};

// The unit that a dimension's name names, in any case, or kUnknown.
CssUnit CssUnitFromName(std::u16string_view name);
// How Blink writes a unit: "px", "%", and nothing for a number or integer.
std::string_view CssUnitName(CssUnit unit);
CssUnitCategory CssUnitCategoryOf(CssUnit unit);
// The unit Blink converts a category to, or kUnknown if it converts none.
CssUnit CanonicalUnitFor(CssUnitCategory category);
// What a value in `unit` is multiplied by to give it in its category's
// canonical unit, and 1 where there is none.
double ConversionToCanonicalUnits(CssUnit unit);
bool IsLengthUnit(CssUnit unit);
// A length relative to a font, the viewport or a container.
bool IsRelativeLengthUnit(CssUnit unit);
bool IsResolutionUnit(CssUnit unit);

enum class CssBlockType : uint8_t {
  kNotBlock,
  kBlockStart,
  kBlockEnd,
};

enum class CssNumericValueType : uint8_t {
  kInteger,
  kNumber,
};

enum class CssNumericSign : uint8_t {
  kNoSign,
  kPlus,
  kMinus,
};

enum class CssHashType : uint8_t {
  kId,
  kUnrestricted,
};

struct CssToken {
  CssToken();
  CssToken(const CssToken&);
  CssToken(CssToken&&);
  CssToken& operator=(const CssToken&);
  CssToken& operator=(CssToken&&);
  ~CssToken();

  friend bool operator==(const CssToken&, const CssToken&) = default;

  CssTokenType type = CssTokenType::kEndOfFile;
  CssBlockType block_type = CssBlockType::kNotBlock;

  // An ident's, function's, at-keyword's, hash's, URL's or string's value,
  // and a dimension's unit as written.
  std::u16string value;
  // A delimiter's code point.
  char32_t delimiter = 0;
  // A number's, percentage's or dimension's value, held between the largest
  // floats either way, as Blink holds it.
  double numeric_value = 0;
  CssNumericValueType numeric_value_type = CssNumericValueType::kInteger;
  CssNumericSign numeric_sign = CssNumericSign::kNoSign;
  // kNumber for a number, kPercentage for a percentage, and a dimension's
  // unit, kUnknown for one that Blink does not know.
  CssUnit unit = CssUnit::kUnknown;
  CssHashType hash_type = CssHashType::kUnrestricted;
  char32_t unicode_range_start = 0;
  char32_t unicode_range_end = 0;

  // Where the token starts and ends in the input, in code units. Comments
  // that the tokenizer skips lie between one token's end and the next one's
  // start.
  size_t start = 0;
  size_t end = 0;
};

}  // namespace mitto

#endif  // MITTO_CSS_CSS_TOKEN_H_
