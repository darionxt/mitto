#include "mitto/css/media_query_parser.h"

#include <array>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/compiler_specific.h"
#include "base/memory/raw_ref.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/css/css_token_stream.h"

namespace mitto {

namespace {

using NodePtr = std::unique_ptr<ConditionalExpNode>;

std::u16string ToAsciiLower(std::u16string_view text) {
  std::u16string lower(text);
  for (char16_t& c : lower) {
    c = base::ToLowerASCII(c);
  }
  return lower;
}

bool EqualsIgnoringAsciiCase(std::u16string_view a, std::string_view b) {
  return base::EqualsCaseInsensitiveASCII(a, base::ASCIIToUTF16(b));
}

bool IsOneOf(std::u16string_view name,
             std::initializer_list<std::string_view> names) {
  for (std::string_view candidate : names) {
    if (name == base::ASCIIToUTF16(candidate)) {
      return true;
    }
  }
  return false;
}

bool IsValidVariableName(std::u16string_view name) {
  return name.size() >= 3 && name[0] == '-' && name[1] == '-';
}

// As Blink's css_parsing_utils::AtIdent().
bool AtIdent(const CssToken& token, std::string_view ident) {
  return token.type == CssTokenType::kIdent &&
         EqualsIgnoringAsciiCase(token.value, ident);
}

bool ConsumeIfIdent(CssTokenStream& stream, std::string_view ident) {
  if (!AtIdent(stream.Peek(), ident)) {
    return false;
  }
  stream.ConsumeIncludingWhitespace();
  return true;
}

bool AtDelimiter(const CssToken& token, char32_t c) {
  return token.type == CssTokenType::kDelimiter && token.delimiter == c;
}

bool ConsumeIfDelimiter(CssTokenStream& stream, char32_t c) {
  if (!AtDelimiter(stream.Peek(), c)) {
    return false;
  }
  stream.ConsumeIncludingWhitespace();
  return true;
}

// As Blink's IsTokenAllowedForAnyValue().
bool IsTokenAllowedForAnyValue(const CssToken& token) {
  switch (token.type) {
    case CssTokenType::kBadString:
    case CssTokenType::kEndOfFile:
    case CssTokenType::kBadUrl:
      return false;
    case CssTokenType::kRightParenthesis:
    case CssTokenType::kRightBracket:
    case CssTokenType::kRightBrace:
      return token.block_type == CssBlockType::kBlockEnd;
    default:
      return true;
  }
}

void ConsumeAnyValue(CssTokenStream& stream);

bool ConsumeAnyComponentValue(CssTokenStream& stream) {
  if (stream.Peek().block_type == CssBlockType::kBlockStart) {
    CssTokenStream::RestoringBlockGuard guard(stream);
    ConsumeAnyValue(stream);
    if (guard.Release()) {
      return true;
    }
  } else if (IsTokenAllowedForAnyValue(stream.Peek())) {
    stream.Consume();
    return true;
  }
  return false;
}

// As Blink's css_parsing_utils::ConsumeAnyValue().
void ConsumeAnyValue(CssTokenStream& stream) {
  while (!stream.AtEnd()) {
    if (!ConsumeAnyComponentValue(stream)) {
      return;
    }
  }
}

// The keywords that each media feature takes.
// As Blink's FeatureWithValidIdent().
bool FeatureWithValidIdent(std::u16string_view feature,
                           std::u16string_view ident,
                           const MediaQueryFeatures& features) {
  if (feature == u"display-mode") {
    return IsOneOf(ident, {"fullscreen", "unframed", "standalone", "minimal-ui",
                           "window-controls-overlay", "browser", "tabbed",
                           "picture-in-picture"});
  }
  if (features.additional_windowing_controls && feature == u"display-state") {
    return IsOneOf(ident, {"fullscreen", "normal", "minimized", "maximized"});
  }
  if (features.additional_windowing_controls && feature == u"resizable") {
    return IsOneOf(ident, {"true", "false"});
  }
  if (feature == u"orientation") {
    return IsOneOf(ident, {"portrait", "landscape"});
  }
  if (feature == u"pointer" || feature == u"any-pointer") {
    return IsOneOf(ident, {"none", "coarse", "fine"});
  }
  if (feature == u"hover" || feature == u"any-hover") {
    return IsOneOf(ident, {"none", "hover"});
  }
  if (feature == u"scan") {
    return IsOneOf(ident, {"interlace", "progressive"});
  }
  if (feature == u"color-gamut") {
    return IsOneOf(ident, {"srgb", "p3", "rec2020"});
  }
  if (features.inverted_colors && feature == u"inverted-colors") {
    return IsOneOf(ident, {"inverted", "none"});
  }
  if (feature == u"prefers-color-scheme") {
    return IsOneOf(ident, {"dark", "light"});
  }
  if (feature == u"prefers-contrast") {
    return IsOneOf(ident, {"no-preference", "more", "less", "custom"});
  }
  if (feature == u"prefers-reduced-motion") {
    return IsOneOf(ident, {"no-preference", "reduce"});
  }
  if (feature == u"dynamic-range") {
    return IsOneOf(ident, {"standard", "high"});
  }
  if (features.video_dynamic_range && feature == u"video-dynamic-range") {
    return IsOneOf(ident, {"standard", "high"});
  }
  if (features.prefers_reduced_data && feature == u"prefers-reduced-data") {
    return IsOneOf(ident, {"no-preference", "reduce"});
  }
  if (feature == u"prefers-reduced-transparency") {
    return IsOneOf(ident, {"no-preference", "reduce"});
  }
  if (features.forced_colors && feature == u"forced-colors") {
    return IsOneOf(ident, {"none", "active"});
  }
  if (features.navigation_controls && feature == u"navigation-controls") {
    return IsOneOf(ident, {"none", "back-button"});
  }
  if (features.device_posture && feature == u"device-posture") {
    return IsOneOf(ident, {"continuous", "folded"});
  }
  if (feature == u"overflow-inline") {
    return IsOneOf(ident, {"none", "scroll"});
  }
  if (feature == u"overflow-block") {
    return IsOneOf(ident, {"none", "scroll", "paged"});
  }
  if (feature == u"update") {
    return IsOneOf(ident, {"none", "fast", "slow"});
  }
  if (feature == u"scripting") {
    return IsOneOf(ident, {"enabled", "initial-only", "none"});
  }
  return false;
}

bool FeatureExpectingInteger(std::u16string_view feature,
                             const MediaQueryFeatures& features) {
  if (IsOneOf(feature, {"color", "max-color", "min-color", "color-index",
                        "max-color-index", "min-color-index", "monochrome",
                        "max-monochrome", "min-monochrome"})) {
    return true;
  }
  return features.viewport_segments &&
         IsOneOf(feature, {"horizontal-viewport-segments",
                           "vertical-viewport-segments"});
}

bool FeatureWithAspectRatio(std::u16string_view feature) {
  return IsOneOf(feature,
                 {"aspect-ratio", "device-aspect-ratio", "min-aspect-ratio",
                  "max-aspect-ratio", "min-device-aspect-ratio",
                  "max-device-aspect-ratio"});
}

bool FeatureWithValidLength(std::u16string_view feature,
                            const CssPrimitiveValue& value) {
  const std::optional<double> known = value.GetValueIfKnown();
  if (!(value.IsLength() || (value.IsNumber() && known == 0.0))) {
    return false;
  }
  return IsOneOf(feature,
                 {"height", "max-height", "min-height", "width", "max-width",
                  "min-width", "block-size", "max-block-size", "min-block-size",
                  "inline-size", "max-inline-size", "min-inline-size",
                  "device-height", "max-device-height", "min-device-height",
                  "device-width", "min-device-width", "max-device-width"});
}

bool FeatureWithValidDensity(std::u16string_view feature,
                             const CssPrimitiveValue& value) {
  // A resolution is never negative.
  const std::optional<double> known = value.GetValueIfKnown();
  if (!value.IsResolution() || (known && *known < 0.0)) {
    return false;
  }
  return IsOneOf(feature, {"resolution", "min-resolution", "max-resolution"});
}

bool FeatureWithNumber(std::u16string_view feature,
                       const CssPrimitiveValue& value) {
  return value.IsNumber() &&
         IsOneOf(feature, {"-webkit-transform-3d", "-webkit-device-pixel-ratio",
                           "-webkit-max-device-pixel-ratio",
                           "-webkit-min-device-pixel-ratio"});
}

bool FeatureWithZeroOrOne(std::u16string_view feature,
                          const CssPrimitiveValue& value) {
  const std::optional<double> known = value.GetValueIfKnown();
  if (!value.IsInteger() || (known && *known != 1.0 && *known != 0.0)) {
    return false;
  }
  return feature == u"grid";
}

// A math function of `category` at the next token, or nullopt with nothing
// consumed.
// As Blink's MathFunctionParser.
std::optional<CssPrimitiveValue> ConsumeMath(CssTokenStream& stream,
                                             CssCalcCategory category,
                                             bool integer,
                                             bool non_negative) {
  const CssTokenStream::State savepoint = stream.Save();
  std::optional<CssMathFunction> math =
      CssMathFunction::Consume(stream, integer, non_negative);
  if (!math || math->Category() != category) {
    stream.Restore(savepoint);
    return std::nullopt;
  }
  return CssPrimitiveValue::Math(std::move(*math));
}

// As Blink's css_parsing_utils::ConsumeInteger().
std::optional<CssPrimitiveValue> ConsumeInteger(CssTokenStream& stream) {
  const CssToken& token = stream.Peek();
  if (token.type == CssTokenType::kNumber) {
    if (token.numeric_value_type == CssNumericValueType::kNumber) {
      return std::nullopt;
    }
    return CssPrimitiveValue::Literal(
        stream.ConsumeIncludingWhitespace().numeric_value, CssUnit::kInteger);
  }
  return ConsumeMath(stream, CssCalcCategory::kNumber, /*integer=*/true,
                     /*non_negative=*/false);
}

// As Blink's css_parsing_utils::ConsumeNumber().
std::optional<CssPrimitiveValue> ConsumeNumber(CssTokenStream& stream,
                                               bool non_negative) {
  const CssToken& token = stream.Peek();
  if (token.type == CssTokenType::kNumber) {
    if (non_negative && token.numeric_value < 0) {
      return std::nullopt;
    }
    return CssPrimitiveValue::Literal(
        stream.ConsumeIncludingWhitespace().numeric_value, CssUnit::kNumber);
  }
  return ConsumeMath(stream, CssCalcCategory::kNumber, /*integer=*/false,
                     non_negative);
}

// In standard mode, which a media query is parsed in.
// As Blink's css_parsing_utils::ConsumeLength().
std::optional<CssPrimitiveValue> ConsumeLength(CssTokenStream& stream) {
  const CssToken& token = stream.Peek();
  if (token.type == CssTokenType::kDimension) {
    if (!IsLengthUnit(token.unit) || token.unit == CssUnit::kQuirkyEms) {
      return std::nullopt;
    }
    const CssUnit unit = token.unit;
    return CssPrimitiveValue::Literal(
        stream.ConsumeIncludingWhitespace().numeric_value, unit);
  }
  if (token.type == CssTokenType::kNumber) {
    if (token.numeric_value != 0) {
      return std::nullopt;
    }
    return CssPrimitiveValue::Literal(
        stream.ConsumeIncludingWhitespace().numeric_value, CssUnit::kPixels);
  }
  return ConsumeMath(stream, CssCalcCategory::kLength, /*integer=*/false,
                     /*non_negative=*/false);
}

// As Blink's css_parsing_utils::ConsumeResolution().
std::optional<CssPrimitiveValue> ConsumeResolution(CssTokenStream& stream) {
  const CssToken& token = stream.Peek();
  if (token.type == CssTokenType::kDimension) {
    if (!IsResolutionUnit(token.unit) || token.numeric_value < 0.0) {
      return std::nullopt;
    }
    const CssUnit unit = token.unit;
    return CssPrimitiveValue::Literal(
        stream.ConsumeIncludingWhitespace().numeric_value, unit);
  }
  return ConsumeMath(stream, CssCalcCategory::kResolution, /*integer=*/false,
                     /*non_negative=*/true);
}

// As Blink's MediaQueryExpValue::Consume().
std::optional<MediaQueryExpValue> ConsumeExpValue(
    std::u16string_view feature,
    CssTokenStream& stream,
    const MediaQueryFeatures& features) {
  std::optional<CssPrimitiveValue> value = ConsumeInteger(stream);
  if (!value && !FeatureExpectingInteger(feature, features)) {
    value = ConsumeNumber(stream, /*non_negative=*/false);
  }
  if (!value) {
    value = ConsumeLength(stream);
  }
  if (!value) {
    value = ConsumeResolution(stream);
  }

  if (!value) {
    const CssToken& token = stream.Peek();
    if (token.type != CssTokenType::kIdent) {
      return std::nullopt;
    }
    std::u16string ident = ToAsciiLower(token.value);
    if (!FeatureWithValidIdent(feature, ident, features)) {
      return std::nullopt;
    }
    stream.ConsumeIncludingWhitespace();
    return MediaQueryExpValue(std::move(ident));
  }

  if (FeatureWithAspectRatio(feature)) {
    const std::optional<double> known = value->GetValueIfKnown();
    if (known && *known < 0.0) {
      return std::nullopt;
    }
    if (!AtDelimiter(stream.Peek(), '/')) {
      return MediaQueryExpValue(
          std::move(*value), CssPrimitiveValue::Literal(1, CssUnit::kNumber));
    }
    stream.ConsumeIncludingWhitespace();
    std::optional<CssPrimitiveValue> denominator =
        ConsumeNumber(stream, /*non_negative=*/true);
    if (!denominator) {
      return std::nullopt;
    }
    if (value->GetValueIfKnown() == 0.0 &&
        denominator->GetValueIfKnown() == 0.0) {
      return MediaQueryExpValue(
          CssPrimitiveValue::Literal(1, CssUnit::kNumber),
          CssPrimitiveValue::Literal(0, CssUnit::kNumber));
    }
    return MediaQueryExpValue(std::move(*value), std::move(*denominator));
  }

  if ((value->IsInteger() && FeatureExpectingInteger(feature, features)) ||
      FeatureWithNumber(feature, *value) ||
      FeatureWithZeroOrOne(feature, *value) ||
      FeatureWithValidLength(feature, *value) ||
      FeatureWithValidDensity(feature, *value)) {
    return MediaQueryExpValue(std::move(*value));
  }
  return std::nullopt;
}

bool IsComparisonDelimiter(char32_t c) {
  return c == '<' || c == '>' || c == '=';
}

bool IsLtLe(MediaQueryOperator op) {
  return op == MediaQueryOperator::kLt || op == MediaQueryOperator::kLe;
}

bool IsGtGe(MediaQueryOperator op) {
  return op == MediaQueryOperator::kGt || op == MediaQueryOperator::kGe;
}

class Parser {
 public:
  explicit Parser(const MediaQueryFeatures& features) : features_(features) {}

  std::unique_ptr<MediaQuerySet> ParseImpl(CssTokenStream& stream) {
    stream.ConsumeWhitespace();
    // An empty input gives an empty set rather than "not all".
    if (stream.AtEnd()) {
      return std::make_unique<MediaQuerySet>();
    }
    std::vector<std::unique_ptr<MediaQuery>> queries;
    do {
      std::unique_ptr<MediaQuery> query = ConsumeQuery(stream);
      const bool ok = query && (stream.AtEnd() ||
                                stream.Peek().type == CssTokenType::kComma);
      queries.push_back(ok ? std::move(query) : MediaQuery::CreateNotAll());
    } while (!stream.AtEnd() && ConsumeUntilCommaInclusive(stream));
    return std::make_unique<MediaQuerySet>(std::move(queries));
  }

 private:
  enum class ParseMode {
    kNormal,
    kWithoutOr,
  };

  static bool ConsumeUntilCommaInclusive(CssTokenStream& stream) {
    stream.SkipUntilPeekedTypeIs(CssTokenType::kComma);
    if (stream.Peek().type == CssTokenType::kComma) {
      stream.ConsumeIncludingWhitespace();
      return true;
    }
    return false;
  }

  static void SkipUntilComparisonOrColon(CssTokenStream& stream) {
    while (!stream.AtEnd()) {
      stream.SkipUntilPeekedTypeIs(CssTokenType::kDelimiter,
                                   CssTokenType::kColon);
      if (stream.AtEnd()) {
        return;
      }
      const CssToken& token = stream.Peek();
      if (token.type == CssTokenType::kDelimiter) {
        if (IsComparisonDelimiter(token.delimiter)) {
          return;
        }
        stream.Consume();
      } else {
        return;
      }
    }
  }

  static bool IsRestrictorOrLogicalOperator(const CssToken& token) {
    return EqualsIgnoringAsciiCase(token.value, "not") ||
           EqualsIgnoringAsciiCase(token.value, "and") ||
           EqualsIgnoringAsciiCase(token.value, "or") ||
           EqualsIgnoringAsciiCase(token.value, "only") ||
           EqualsIgnoringAsciiCase(token.value, "layer");
  }

  // As Blink's MediaQueryParser::MediaQueryFeatureSet::IsAllowed().
  bool IsAllowed(std::u16string_view feature) const {
    if (IsOneOf(feature, {"inline-size", "min-inline-size", "max-inline-size",
                          "block-size", "min-block-size", "max-block-size",
                          "stuck", "snapped", "scrollable"}) ||
        (feature == u"scrolled" && features_->scrolled_container_queries) ||
        IsValidVariableName(feature)) {
      return false;
    }
    // Blink parses (fallback: none) for a media query, and its evaluator
    // then reaches a NOTREACHED(). Here it is unknown, as no other feature
    // of a container query is in a media query.
    return feature != u"fallback";
  }

  // As Blink's MediaQueryParser::MediaQueryFeatureSet::IsAllowedWithoutValue().
  bool IsAllowedWithoutValue(std::u16string_view feature) const {
    return IsOneOf(feature, {"monochrome",
                             "color",
                             "color-index",
                             "grid",
                             "height",
                             "width",
                             "device-height",
                             "device-width",
                             "orientation",
                             "aspect-ratio",
                             "device-aspect-ratio",
                             "hover",
                             "any-hover",
                             "-webkit-transform-3d",
                             "pointer",
                             "any-pointer",
                             "-webkit-device-pixel-ratio",
                             "resolution",
                             "display-mode",
                             "scan",
                             "color-gamut",
                             "dynamic-range",
                             "prefers-color-scheme",
                             "prefers-contrast",
                             "prefers-reduced-motion",
                             "overflow-inline",
                             "overflow-block",
                             "update",
                             "prefers-reduced-transparency",
                             "scripting"}) ||
           (feature == u"video-dynamic-range" &&
            features_->video_dynamic_range) ||
           (feature == u"prefers-reduced-data" &&
            features_->prefers_reduced_data) ||
           (feature == u"forced-colors" && features_->forced_colors) ||
           (feature == u"navigation-controls" &&
            features_->navigation_controls) ||
           ((feature == u"horizontal-viewport-segments" ||
             feature == u"vertical-viewport-segments") &&
            features_->viewport_segments) ||
           (feature == u"device-posture" && features_->device_posture) ||
           (feature == u"inverted-colors" && features_->inverted_colors) ||
           ((feature == u"display-state" || feature == u"resizable") &&
            features_->additional_windowing_controls);
  }

  // As Blink's MediaQueryParser::MediaQueryFeatureSet::IsRangeTypeFeature().
  static bool IsRangeTypeFeature(std::u16string_view feature) {
    return IsOneOf(
        feature,
        {"height", "width", "device-height", "device-width", "aspect-ratio",
         "device-aspect-ratio", "resolution", "color", "color-index",
         "monochrome", "-webkit-device-pixel-ratio",
         "horizontal-viewport-segments", "vertical-viewport-segments"});
  }

  // [ not | only ]
  static MediaQuery::RestrictorType ConsumeRestrictor(CssTokenStream& stream) {
    if (ConsumeIfIdent(stream, "not")) {
      return MediaQuery::RestrictorType::kNot;
    }
    if (ConsumeIfIdent(stream, "only")) {
      return MediaQuery::RestrictorType::kOnly;
    }
    return MediaQuery::RestrictorType::kNone;
  }

  // https://drafts.csswg.org/mediaqueries-4/#typedef-media-type
  static std::optional<std::u16string> ConsumeType(CssTokenStream& stream) {
    if (stream.Peek().type != CssTokenType::kIdent) {
      return std::nullopt;
    }
    if (IsRestrictorOrLogicalOperator(stream.Peek())) {
      return std::nullopt;
    }
    return stream.ConsumeIncludingWhitespace().value;
  }

  // https://drafts.csswg.org/mediaqueries-4/#typedef-mf-comparison
  static MediaQueryOperator ConsumeComparison(CssTokenStream& stream) {
    const CssToken& first = stream.Peek();
    if (first.type != CssTokenType::kDelimiter ||
        !IsComparisonDelimiter(first.delimiter)) {
      return MediaQueryOperator::kNone;
    }
    switch (first.delimiter) {
      case '=':
        stream.ConsumeIncludingWhitespace();
        return MediaQueryOperator::kEq;
      case '<':
        stream.Consume();
        if (ConsumeIfDelimiter(stream, '=')) {
          return MediaQueryOperator::kLe;
        }
        stream.ConsumeWhitespace();
        return MediaQueryOperator::kLt;
      case '>':
        stream.Consume();
        if (ConsumeIfDelimiter(stream, '=')) {
          return MediaQueryOperator::kGe;
        }
        stream.ConsumeWhitespace();
        return MediaQueryOperator::kGt;
    }
    return MediaQueryOperator::kNone;
  }

  // https://drafts.csswg.org/mediaqueries-4/#typedef-mf-name, consumed only
  // if a media query takes it.
  std::optional<std::u16string> ConsumeAllowedName(CssTokenStream& stream) {
    if (stream.Peek().type != CssTokenType::kIdent) {
      return std::nullopt;
    }
    std::u16string name = ToAsciiLower(stream.Peek().value);
    if (!IsAllowed(name)) {
      return std::nullopt;
    }
    stream.ConsumeIncludingWhitespace();
    return name;
  }

  std::optional<std::u16string> ConsumeRangeContextFeatureName(
      CssTokenStream& stream) {
    std::optional<std::u16string> name = ConsumeAllowedName(stream);
    if (name && IsRangeTypeFeature(*name)) {
      return name;
    }
    return std::nullopt;
  }

  // https://drafts.csswg.org/mediaqueries-4/#typedef-media-feature
  // Not inlined, nor are ConsumeLeaf() and ConsumeGeneralEnclosed(), so that
  // their locals stay out of the frames of ConsumeCondition() and
  // ConsumeInner(), which recurse once for each parenthesis of a condition, as
  // Blink's do: Chrome takes some 20,000 of them.
  NOINLINE NodePtr ConsumeFeature(CssTokenStream& stream) {
    const CssTokenStream::State start = stream.Save();
    {
      const std::optional<std::u16string> feature_name =
          ConsumeAllowedName(stream);

      // <mf-boolean> = <mf-name>
      if (feature_name && stream.AtEnd() &&
          IsAllowedWithoutValue(*feature_name)) {
        return ConditionalExpNode::Feature(
            MediaQueryExp(*feature_name, MediaQueryExpBounds()));
      }

      // <mf-plain> = <mf-name> : <mf-value>
      if (feature_name && stream.Peek().type == CssTokenType::kColon) {
        stream.ConsumeIncludingWhitespace();
        // An empty <mf-value> is legal here; it fails to convert below.
        std::optional<MediaQueryExpValue> value =
            ConsumeExpValue(*feature_name, stream, *features_);
        if (value && stream.AtEnd()) {
          return ConditionalExpNode::Feature(MediaQueryExp(
              *feature_name,
              MediaQueryExpBounds(MediaQueryExpComparison(std::move(*value)))));
        }
      }
      stream.Restore(start);
    }

    // <mf-range> = <mf-name> <mf-comparison> <mf-value>
    //            | <mf-value> <mf-comparison> <mf-name>
    //            | <mf-value> <mf-lt> <mf-name> <mf-lt> <mf-value>
    //            | <mf-value> <mf-gt> <mf-name> <mf-gt> <mf-value>
    {
      // <mf-name> <mf-comparison> <mf-value>, such as width <= 10px.
      const std::optional<std::u16string> feature_name =
          ConsumeRangeContextFeatureName(stream);
      if (feature_name && !stream.AtEnd()) {
        const MediaQueryOperator op = ConsumeComparison(stream);
        if (op != MediaQueryOperator::kNone) {
          std::optional<MediaQueryExpValue> value =
              ConsumeExpValue(*feature_name, stream, *features_);
          if (value && stream.AtEnd()) {
            return ConditionalExpNode::Feature(MediaQueryExp(
                *feature_name,
                MediaQueryExpBounds(
                    MediaQueryExpComparison(),
                    MediaQueryExpComparison(std::move(*value), op))));
          }
        }
      }
      stream.Restore(start);
    }

    // The <mf-value> comes first, so skip it to find the <mf-name>, and
    // then go back to read it as that feature's:
    //
    //  <value1> <op1> <feature_name> <op2> <value2>
    SkipUntilComparisonOrColon(stream);
    if (stream.AtEnd()) {
      return nullptr;
    }
    const size_t offset_after_value1 = stream.LookAheadOffset();

    const MediaQueryOperator op1 = ConsumeComparison(stream);
    if (op1 == MediaQueryOperator::kNone) {
      return nullptr;
    }
    const std::optional<std::u16string> feature_name =
        ConsumeRangeContextFeatureName(stream);
    if (!feature_name) {
      return nullptr;
    }
    stream.ConsumeWhitespace();
    const CssTokenStream::State after_feature_name = stream.Save();

    stream.Restore(start);
    std::optional<MediaQueryExpValue> value1 =
        ConsumeExpValue(*feature_name, stream, *features_);
    if (!value1) {
      return nullptr;
    }
    if (stream.LookAheadOffset() != offset_after_value1) {
      // There was junk between <value1> and <op1>.
      return nullptr;
    }
    stream.Restore(after_feature_name);

    if (stream.AtEnd()) {
      // <mf-value> <mf-comparison> <mf-name>
      return ConditionalExpNode::Feature(MediaQueryExp(
          *feature_name,
          MediaQueryExpBounds(MediaQueryExpComparison(std::move(*value1), op1),
                              MediaQueryExpComparison())));
    }

    const MediaQueryOperator op2 = ConsumeComparison(stream);
    if (op2 == MediaQueryOperator::kNone) {
      return nullptr;
    }
    // Mixing [lt, le] and [gt, ge] is not allowed by the grammar.
    if (!(IsLtLe(op1) && IsLtLe(op2)) && !(IsGtGe(op1) && IsGtGe(op2))) {
      return nullptr;
    }
    std::optional<MediaQueryExpValue> value2 =
        ConsumeExpValue(*feature_name, stream, *features_);
    if (!value2) {
      return nullptr;
    }
    return ConditionalExpNode::Feature(MediaQueryExp(
        *feature_name,
        MediaQueryExpBounds(MediaQueryExpComparison(std::move(*value1), op1),
                            MediaQueryExpComparison(std::move(*value2), op2))));
  }

  // ( <media-feature> )
  NOINLINE NodePtr ConsumeLeaf(CssTokenStream& stream) {
    stream.ConsumeWhitespace();
    if (NodePtr feature = ConsumeFeature(stream)) {
      stream.ConsumeWhitespace();
      return feature;
    }
    return nullptr;
  }

  // As Blink's ConditionalParser::ConsumeCondition().
  NodePtr ConsumeCondition(CssTokenStream& stream,
                           ParseMode parse_mode = ParseMode::kNormal) {
    if (ConsumeIfIdent(stream, "not")) {
      return ConditionalExpNode::Not(ConsumeInner(stream));
    }
    NodePtr result = ConsumeInner(stream);
    if (AtIdent(stream.Peek(), "and")) {
      while (result && ConsumeIfIdent(stream, "and")) {
        result =
            ConditionalExpNode::And(std::move(result), ConsumeInner(stream));
      }
    } else if (result && AtIdent(stream.Peek(), "or") &&
               parse_mode != ParseMode::kWithoutOr) {
      while (result && ConsumeIfIdent(stream, "or")) {
        result =
            ConditionalExpNode::Or(std::move(result), ConsumeInner(stream));
      }
    }
    return result;
  }

  // As Blink's ConditionalParser::ConsumeInner().
  NodePtr ConsumeInner(CssTokenStream& stream) {
    if (stream.Peek().type == CssTokenType::kLeftParenthesis) {
      CssTokenStream::RestoringBlockGuard guard(stream);
      stream.ConsumeWhitespace();
      const CssTokenStream::State savepoint = stream.Save();
      NodePtr node = ConsumeLeaf(stream);
      if (!node) {
        stream.Restore(savepoint);
        node = ConsumeCondition(stream);
      }
      if (node && guard.Release()) {
        stream.ConsumeWhitespace();
        return ConditionalExpNode::Nested(std::move(node));
      }
    }
    // A media query takes no function, so one is <general-enclosed>.
    return ConsumeGeneralEnclosed(stream);
  }

  // As Blink's ConditionalParser::ConsumeGeneralEnclosed().
  NOINLINE NodePtr ConsumeGeneralEnclosed(CssTokenStream& stream) {
    if (stream.Peek().type != CssTokenType::kLeftParenthesis &&
        stream.Peek().type != CssTokenType::kFunction) {
      return nullptr;
    }
    const size_t start_offset = stream.Offset();
    {
      CssTokenStream::BlockGuard guard(stream);
      stream.ConsumeWhitespace();
      // <any-value> is optional in <general-enclosed>, so an empty block is
      // fine.
      ConsumeAnyValue(stream);
      if (!stream.AtEnd()) {
        return nullptr;
      }
    }
    const size_t end_offset = stream.Offset();
    std::u16string general_enclosed(
        stream.StringRangeAt(start_offset, end_offset - start_offset));
    stream.ConsumeWhitespace();
    return ConditionalExpNode::Unknown(std::move(general_enclosed));
  }

  // https://drafts.csswg.org/mediaqueries-4/#typedef-media-query
  std::unique_ptr<MediaQuery> ConsumeQuery(CssTokenStream& stream) {
    const CssTokenStream::State savepoint = stream.Save();

    // [ not | only ]? <media-type> [ and <media-condition-without-or> ]?
    const MediaQuery::RestrictorType restrictor = ConsumeRestrictor(stream);
    std::optional<std::u16string> type = ConsumeType(stream);
    if (type) {
      if (!ConsumeIfIdent(stream, "and")) {
        return std::make_unique<MediaQuery>(restrictor, std::move(*type),
                                            nullptr);
      }
      if (NodePtr node = ConsumeCondition(stream, ParseMode::kWithoutOr)) {
        return std::make_unique<MediaQuery>(restrictor, std::move(*type),
                                            std::move(node));
      }
      return nullptr;
    }
    stream.Restore(savepoint);

    // Otherwise, <media-condition>.
    if (NodePtr node = ConsumeCondition(stream)) {
      return std::make_unique<MediaQuery>(MediaQuery::RestrictorType::kNone,
                                          u"all", std::move(node));
    }
    return nullptr;
  }

  const raw_ref<const MediaQueryFeatures> features_;
};

}  // namespace

std::unique_ptr<MediaQuerySet> ParseMediaQuerySet(
    std::u16string_view input,
    const MediaQueryFeatures& features) {
  CssTokenStream stream(input);
  return Parser(features).ParseImpl(stream);
}

}  // namespace mitto
