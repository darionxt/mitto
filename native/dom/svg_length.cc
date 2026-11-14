#include "mitto/dom/svg_length.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "base/notreached.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_svg_animated_length.h"
#include "mitto/bindings/v8_svg_length.h"
#include "mitto/css/css_serialization.h"
#include "mitto/css/css_token_stream.h"
#include "mitto/css/css_tokenizer.h"
#include "mitto/css/media_values.h"
#include "mitto/dom/document.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/media_query_list.h"
#include "mitto/dom/svg_element.h"
#include "mitto/dom/window.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

namespace {

// The interface's unit type constants.
constexpr uint16_t kLengthTypeUnknown = 0;
constexpr uint16_t kLengthTypeNumber = 1;
constexpr uint16_t kLengthTypePc = 10;

// CSSPrimitiveValue::ClampToCSSLengthRange(): within what a LayoutUnit holds.
constexpr float kMaxValueForCssLength =
    std::numeric_limits<int>::max() / 64 - 2;

float ClampToCssLengthRange(double value) {
  if (std::isnan(value)) {
    return 0;
  }
  return static_cast<float>(
      std::clamp(value, -static_cast<double>(kMaxValueForCssLength),
                 static_cast<double>(kMaxValueForCssLength)));
}

// The unit of a unit type constant, which IsValidLengthUnit() takes.
std::optional<CssUnit> UnitOfType(uint16_t type) {
  static constexpr std::array<CssUnit, 10> kUnits = {
      CssUnit::kNumber,      CssUnit::kPercentage, CssUnit::kEms,
      CssUnit::kExs,         CssUnit::kPixels,     CssUnit::kCentimeters,
      CssUnit::kMillimeters, CssUnit::kInches,     CssUnit::kPoints,
      CssUnit::kPicas,
  };
  if (type < kLengthTypeNumber || type > kLengthTypePc) {
    return std::nullopt;
  }
  return kUnits[type - kLengthTypeNumber];
}

bool IsAbsoluteUnit(CssUnit unit) {
  switch (unit) {
    case CssUnit::kNumber:
    case CssUnit::kInteger:
    case CssUnit::kPixels:
    case CssUnit::kCentimeters:
    case CssUnit::kMillimeters:
    case CssUnit::kQuarterMillimeters:
    case CssUnit::kInches:
    case CssUnit::kPoints:
    case CssUnit::kPicas:
      return true;
    default:
      return false;
  }
}

bool IsContainerUnit(CssUnit unit) {
  switch (unit) {
    case CssUnit::kContainerWidth:
    case CssUnit::kContainerHeight:
    case CssUnit::kContainerInlineSize:
    case CssUnit::kContainerBlockSize:
    case CssUnit::kContainerMin:
    case CssUnit::kContainerMax:
      return true;
    default:
      return false;
  }
}

bool IsRelativeUnit(CssUnit unit) {
  return unit == CssUnit::kPercentage || IsRelativeLengthUnit(unit);
}

// What relative units resolve against for an element in a document: the
// window and the initial font, as a media query's do. An element that is in
// none has no style, so its fonts are of no size.
CssLengthResolver ResolverFor(const SVGElement* element) {
  if (element) {
    if (Window* const window = element->GetDocument().DefaultView()) {
      if (element->IsConnected()) {
        return MediaValuesFor(window->Start()).LengthResolver();
      }
      CssLengthResolver resolver{};
      resolver.em = resolver.rem = resolver.ex = resolver.rex = 0;
      resolver.ch = resolver.rch = resolver.ic = resolver.ric = 0;
      const MediaValues values = MediaValuesFor(window->Start());
      resolver.viewport_width = values.viewport_width;
      resolver.viewport_height = values.viewport_height;
      return resolver;
    }
  }
  CssLengthResolver resolver{};
  resolver.em = resolver.rem = resolver.ex = resolver.rex = 0;
  resolver.ch = resolver.rch = resolver.ic = resolver.ric = 0;
  return resolver;
}

float ResolveToUserUnits(const SVGLengthValue& value,
                         const SVGElement* context,
                         SVGLengthMode mode);

// SVGViewportResolver: the size of the viewport that a percentage of
// `element`'s takes, from the nearest svg element above it: its viewBox, or
// else its width and height. An outermost svg element's size is its layout
// box's in Blink, which the window stands for here.
std::pair<float, float> ViewportOf(const SVGElement& element) {
  for (Node* node = element.ParentNode(); node; node = node->ParentNode()) {
    if (!node->IsElementNode()) {
      continue;
    }
    Element* const ancestor = static_cast<Element*>(node);
    if (!ancestor->NamespaceURI() ||
        *ancestor->NamespaceURI() != html_names::kSvgNamespace ||
        ancestor->LocalName() != u"svg") {
      continue;
    }
    const SVGElement& svg = static_cast<const SVGElement&>(*ancestor);
    if (const std::u16string* view_box = svg.FindAttribute(u"viewBox")) {
      const std::optional<std::vector<float>> numbers =
          ParseSVGNumberList(*view_box);
      if (numbers && numbers->size() == 4 && (*numbers)[2] >= 0 &&
          (*numbers)[3] >= 0) {
        return {(*numbers)[2], (*numbers)[3]};
      }
    }
    auto length = [&svg](std::u16string_view name, SVGLengthMode mode) {
      const std::u16string* text = svg.FindAttribute(name);
      std::optional<SVGLengthValue> value =
          text ? SVGLengthValue::Parse(*text) : std::nullopt;
      return ResolveToUserUnits(
          value.value_or(SVGLengthValue::Literal(100, CssUnit::kPercentage)),
          &svg, mode);
    };
    return {length(u"width", SVGLengthMode::kWidth),
            length(u"height", SVGLengthMode::kHeight)};
  }
  const CssLengthResolver resolver = ResolverFor(&element);
  return {static_cast<float>(resolver.viewport_width),
          static_cast<float>(resolver.viewport_height)};
}

float ViewportDimension(const SVGElement& element, SVGLengthMode mode) {
  const auto [width, height] = ViewportOf(element);
  switch (mode) {
    case SVGLengthMode::kWidth:
      return width;
    case SVGLengthMode::kHeight:
      return height;
    case SVGLengthMode::kOther:
      return std::sqrt((width * width + height * height) / 2);
  }
  NOTREACHED();
}

// ConvertValueToUserUnits(): `value` in `unit` in user units.
float ConvertToUserUnits(float value,
                         SVGLengthMode mode,
                         CssUnit unit,
                         const SVGElement* context) {
  if (IsAbsoluteUnit(unit)) {
    return ClampToCssLengthRange(value * ConversionToCanonicalUnits(unit));
  }
  if (!context) {
    return 0;
  }
  if (unit == CssUnit::kPercentage) {
    return ClampToCssLengthRange(value * ViewportDimension(*context, mode) /
                                 100);
  }
  return ClampToCssLengthRange(
      ResolverFor(context).ComputedPixels(value, unit));
}

// ConvertValueFromUserUnits(): user units in `unit`.
float ConvertFromUserUnits(float value,
                           SVGLengthMode mode,
                           CssUnit unit,
                           const SVGElement* context) {
  if (IsAbsoluteUnit(unit)) {
    value /= ConversionToCanonicalUnits(unit);
    return value;
  }
  if (!context) {
    return 0;
  }
  if (unit == CssUnit::kPercentage) {
    const float dimension = ViewportDimension(*context, mode);
    if (!dimension) {
      return 0;
    }
    return value * 100 / dimension;
  }
  const double reference = ResolverFor(context).ComputedPixels(1, unit);
  if (!reference) {
    return 0;
  }
  return static_cast<float>(
      std::clamp(value / reference,
                 static_cast<double>(std::numeric_limits<float>::lowest()),
                 static_cast<double>(std::numeric_limits<float>::max())));
}

float ResolveToUserUnits(const SVGLengthValue& value,
                         const SVGElement* context,
                         SVGLengthMode mode) {
  if (value.IsUnparsed()) {
    return 0;
  }
  if (value.IsNumeric()) {
    return ConvertToUserUnits(static_cast<float>(value.Number()), mode,
                              value.Unit(), context);
  }
  if (!context) {
    return 0;
  }
  return ClampToCssLengthRange(value.ComputeMath(ResolverFor(context)));
}

// Blink's EnsureResolvable(): a relative length needs an element in a
// document.
bool IsResolvable(bool relative, const SVGElement* context) {
  return !relative || (context && context->IsConnected());
}

void ThrowUnresolvable(bindings::ExceptionState& exception_state) {
  exception_state.ThrowDOMException("NotSupportedError",
                                    "Could not resolve relative length.");
}

// ConsumeUnparsedDeclaration() for a value that must hold var(): no bad
// token, no closing bracket of no block, no semicolon outside a block, and no
// !important.
bool IsUnparsedDeclarationWithVariable(std::u16string_view text) {
  bool has_variable = false;
  int depth = 0;
  bool after_bang = false;
  for (const CssToken& token : TokenizeCss(text)) {
    switch (token.type) {
      case CssTokenType::kEndOfFile:
        continue;
      case CssTokenType::kBadString:
      case CssTokenType::kBadUrl:
        return false;
      case CssTokenType::kSemicolon:
        if (depth == 0) {
          return false;
        }
        break;
      case CssTokenType::kFunction:
        if (base::EqualsCaseInsensitiveASCII(token.value, u"var")) {
          has_variable = true;
        }
        break;
      case CssTokenType::kIdent:
        if (after_bang &&
            base::EqualsCaseInsensitiveASCII(token.value, u"important")) {
          return false;
        }
        break;
      default:
        break;
    }
    if (token.type != CssTokenType::kWhitespace) {
      after_bang = token.type == CssTokenType::kDelimiter &&
                   token.delimiter == '!' && depth == 0;
    }
    if (token.block_type == CssBlockType::kBlockStart) {
      ++depth;
    } else if (token.block_type == CssBlockType::kBlockEnd) {
      if (depth == 0) {
        return false;
      }
      --depth;
    }
  }
  return has_variable;
}

}  // namespace

SVGLengthValue::SVGLengthValue(Kind kind) : kind_(kind) {}
SVGLengthValue::SVGLengthValue(const SVGLengthValue&) = default;
SVGLengthValue& SVGLengthValue::operator=(const SVGLengthValue&) = default;
SVGLengthValue::~SVGLengthValue() = default;

// static
SVGLengthValue SVGLengthValue::Literal(double value, CssUnit unit) {
  SVGLengthValue length(Kind::kLiteral);
  length.value_ = value;
  length.unit_ = unit;
  return length;
}

// static
SVGLengthValue SVGLengthValue::UnknownNumber(double value) {
  SVGLengthValue length = Literal(value, CssUnit::kNumber);
  length.unknown_number_ = true;
  return length;
}

// static
std::optional<SVGLengthValue> SVGLengthValue::Parse(std::u16string_view text) {
  {
    CssTokenStream stream(text);
    stream.ConsumeWhitespace();
    const CssToken& token = stream.Peek();
    std::optional<SVGLengthValue> length;
    if (token.type == CssTokenType::kFunction) {
      std::optional<CssMathFunction> math = CssMathFunction::Consume(
          stream, /*integer=*/false, /*non_negative=*/false);
      if (math && (math->Category() == CssCalcCategory::kLength ||
                   math->Category() == CssCalcCategory::kNumber)) {
        length = SVGLengthValue(Kind::kMath);
        length->math_ = std::move(*math);
      }
    } else if (token.type == CssTokenType::kNumber ||
               token.type == CssTokenType::kPercentage ||
               (token.type == CssTokenType::kDimension &&
                IsLengthUnit(token.unit) &&
                token.unit != CssUnit::kQuirkyEms)) {
      // SVG's attribute mode takes a number as user units.
      length = Literal(token.numeric_value, token.type == CssTokenType::kNumber
                                                ? CssUnit::kNumber
                                                : token.unit);
      stream.Consume();
    }
    if (length) {
      stream.ConsumeWhitespace();
      if (stream.Peek().type == CssTokenType::kEndOfFile) {
        return length;
      }
    }
  }
  if (!IsUnparsedDeclarationWithVariable(text)) {
    return std::nullopt;
  }
  SVGLengthValue length(Kind::kUnparsed);
  length.unparsed_ = std::u16string(
      base::TrimWhitespace(std::u16string(text), base::TRIM_ALL));
  return length;
}

uint16_t SVGLengthValue::UnitType() const {
  if (kind_ != Kind::kLiteral || unknown_number_) {
    return kLengthTypeUnknown;
  }
  for (uint16_t type = kLengthTypeNumber; type <= kLengthTypePc; ++type) {
    if (UnitOfType(type) == unit_) {
      return type;
    }
  }
  return kLengthTypeUnknown;
}

bool SVGLengthValue::IsRelative() const {
  return kind_ == Kind::kLiteral && IsRelativeUnit(unit_);
}

std::u16string SVGLengthValue::CssText() const {
  switch (kind_) {
    case Kind::kLiteral:
      return NumericLiteralCssText(value_, unit_);
    case Kind::kMath:
      return math_->CssText();
    case Kind::kUnparsed:
      return unparsed_;
  }
  NOTREACHED();
}

double SVGLengthValue::ComputeMath(const CssLengthResolver& resolver) const {
  return math_ ? math_->ComputeValueInCanonicalUnit(resolver) : 0;
}

// static
SVGLength* SVGLength::Create() {
  return bindings::MakeGarbageCollected<SVGLength>(nullptr, false);
}

// static
bindings::ScriptWrappable* SVGLength::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<SVGLength>(
      info, bindings::V8SVGLength::kWrapperTypeInfo, nullptr, false);
}

// static
SVGLength* SVGLength::CreateTearOff(SVGAnimatedLength& owner,
                                    bool is_anim_val) {
  return bindings::MakeGarbageCollected<SVGLength>(&owner, is_anim_val);
}

SVGLength::SVGLength(SVGAnimatedLength* owner, bool is_anim_val)
    : owner_(owner),
      is_anim_val_(is_anim_val),
      own_value_(SVGLengthValue::Literal(0, CssUnit::kNumber)) {}

SVGLength::~SVGLength() = default;

const bindings::WrapperTypeInfo* SVGLength::GetWrapperTypeInfo() const {
  return &bindings::V8SVGLength::kWrapperTypeInfo;
}

void SVGLength::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(owner_);
}

SVGLengthValue SVGLength::Target() const {
  return owner_ ? owner_->BaseValue() : own_value_;
}

void SVGLength::Commit(const SVGLengthValue& value) {
  if (owner_) {
    owner_->SetBaseValue(value);
  } else {
    own_value_ = value;
  }
}

SVGElement* SVGLength::ContextElement() const {
  return owner_ ? &owner_->Element() : nullptr;
}

SVGLengthMode SVGLength::Mode() const {
  return owner_ ? owner_->Mode() : SVGLengthMode::kOther;
}

bool SVGLength::ThrowIfImmutable(
    bindings::ExceptionState& exception_state) const {
  if (!is_anim_val_) {
    return false;
  }
  exception_state.ThrowDOMException("NoModificationAllowedError",
                                    "This object is read-only.");
  return true;
}

uint16_t SVGLength::UnitType() const {
  return Target().UnitType();
}

float SVGLength::Value(bindings::ExceptionState& exception_state) const {
  const SVGLengthValue target = Target();
  // Blink gives 0 for var(), which it does not resolve here.
  if (target.IsUnparsed()) {
    return 0;
  }
  if (!IsResolvable(target.IsRelative(), ContextElement())) {
    ThrowUnresolvable(exception_state);
    return 0;
  }
  return ResolveToUserUnits(target, ContextElement(), Mode());
}

void SVGLength::SetValue(float value,
                         bindings::ExceptionState& exception_state) {
  if (ThrowIfImmutable(exception_state)) {
    return;
  }
  const SVGLengthValue target = Target();
  if (!target.IsNumeric() || IsContainerUnit(target.Unit())) {
    Commit(SVGLengthValue::Literal(value, CssUnit::kNumber));
    return;
  }
  if (!IsResolvable(target.IsRelative(), ContextElement())) {
    ThrowUnresolvable(exception_state);
    return;
  }
  Commit(SVGLengthValue::Literal(
      ConvertFromUserUnits(value, Mode(), target.Unit(), ContextElement()),
      target.Unit()));
}

float SVGLength::ValueInSpecifiedUnits() const {
  const SVGLengthValue target = Target();
  return target.IsNumeric() ? static_cast<float>(target.Number()) : 0;
}

void SVGLength::SetValueInSpecifiedUnits(
    float value,
    bindings::ExceptionState& exception_state) {
  if (ThrowIfImmutable(exception_state)) {
    return;
  }
  const SVGLengthValue target = Target();
  Commit(SVGLengthValue::Literal(
      value, target.IsNumeric() ? target.Unit() : CssUnit::kNumber));
}

std::u16string SVGLength::ValueAsString() const {
  return Target().CssText();
}

void SVGLength::SetValueAsString(const std::u16string& value,
                                 bindings::ExceptionState& exception_state) {
  if (ThrowIfImmutable(exception_state)) {
    return;
  }
  const std::optional<SVGLengthValue> parsed = SVGLengthValue::Parse(value);
  if (!parsed) {
    exception_state.ThrowDOMException(
        "SyntaxError",
        base::StrCat({"The value provided ('", base::UTF16ToUTF8(value),
                      "') is invalid."}));
    return;
  }
  Commit(*parsed);
}

void SVGLength::NewValueSpecifiedUnits(
    uint16_t unit_type,
    float value_in_specified_units,
    bindings::ExceptionState& exception_state) {
  if (ThrowIfImmutable(exception_state)) {
    return;
  }
  const std::optional<CssUnit> unit = UnitOfType(unit_type);
  if (!unit) {
    exception_state.ThrowDOMException(
        "NotSupportedError",
        base::StrCat({"Cannot set value with unknown or invalid units (",
                      base::NumberToString(unit_type), ")."}));
    return;
  }
  Commit(SVGLengthValue::Literal(value_in_specified_units, *unit));
}

void SVGLength::ConvertToSpecifiedUnits(
    uint16_t unit_type,
    bindings::ExceptionState& exception_state) {
  if (ThrowIfImmutable(exception_state)) {
    return;
  }
  const std::optional<CssUnit> unit = UnitOfType(unit_type);
  if (!unit) {
    exception_state.ThrowDOMException(
        "NotSupportedError",
        base::StrCat({"Cannot convert to unknown or invalid units (",
                      base::NumberToString(unit_type), ")."}));
    return;
  }
  const SVGLengthValue target = Target();
  if (target.IsUnparsed() ||
      !IsResolvable(target.IsRelative() || IsRelativeUnit(*unit),
                    ContextElement())) {
    ThrowUnresolvable(exception_state);
    return;
  }
  const float user_units = ResolveToUserUnits(target, ContextElement(), Mode());
  Commit(SVGLengthValue::Literal(
      ConvertFromUserUnits(user_units, Mode(), *unit, ContextElement()),
      *unit));
}

// static
SVGAnimatedLength* SVGAnimatedLength::Create(SVGElement& element,
                                             std::u16string_view attribute,
                                             SVGLengthMode mode,
                                             Initial initial,
                                             bool text_length) {
  return bindings::MakeGarbageCollected<SVGAnimatedLength>(
      element, attribute, mode, initial, text_length);
}

SVGAnimatedLength::SVGAnimatedLength(SVGElement& element,
                                     std::u16string_view attribute,
                                     SVGLengthMode mode,
                                     Initial initial,
                                     bool text_length)
    : SVGAnimatedPropertyBase(element, attribute, SVGPairHalf::kNone),
      mode_(mode),
      initial_(initial),
      text_length_(text_length),
      value_(SVGLengthValue::Literal(0, CssUnit::kNumber)) {}

SVGAnimatedLength::~SVGAnimatedLength() = default;

const bindings::WrapperTypeInfo* SVGAnimatedLength::GetWrapperTypeInfo() const {
  return &bindings::V8SVGAnimatedLength::kWrapperTypeInfo;
}

void SVGAnimatedLength::Trace(cppgc::Visitor* visitor) const {
  SVGAnimatedPropertyBase::Trace(visitor);
  visitor->Trace(base_val_);
  visitor->Trace(anim_val_);
}

SVGLength* SVGAnimatedLength::BaseVal() {
  if (!base_val_) {
    base_val_ = SVGLength::CreateTearOff(*this, /*is_anim_val=*/false);
  }
  return base_val_.Get();
}

SVGLength* SVGAnimatedLength::AnimVal() {
  if (!anim_val_) {
    anim_val_ = SVGLength::CreateTearOff(*this, /*is_anim_val=*/true);
  }
  return anim_val_.Get();
}

const SVGLengthValue& SVGAnimatedLength::BaseValue() {
  EnsureFresh();
  return value_;
}

void SVGAnimatedLength::SetBaseValue(const SVGLengthValue& value) {
  EnsureFresh();
  value_ = value;
  SynchronizeAttribute(value_.CssText());
}

void SVGAnimatedLength::Parse(const std::u16string* value) {
  if (!value && text_length_) {
    value_ = SVGLengthValue::UnknownNumber(0);
    return;
  }
  if (value) {
    if (std::optional<SVGLengthValue> parsed = SVGLengthValue::Parse(*value)) {
      value_ = *parsed;
      return;
    }
  }
  switch (initial_) {
    case Initial::kUnitlessZero:
      value_ = SVGLengthValue::Literal(0, CssUnit::kNumber);
      return;
    case Initial::kPercentMinus10:
      value_ = SVGLengthValue::Literal(-10, CssUnit::kPercentage);
      return;
    case Initial::kPercent0:
      value_ = SVGLengthValue::Literal(0, CssUnit::kPercentage);
      return;
    case Initial::kPercent50:
      value_ = SVGLengthValue::Literal(50, CssUnit::kPercentage);
      return;
    case Initial::kPercent100:
      value_ = SVGLengthValue::Literal(100, CssUnit::kPercentage);
      return;
    case Initial::kPercent120:
      value_ = SVGLengthValue::Literal(120, CssUnit::kPercentage);
      return;
    case Initial::kNumber3:
      value_ = SVGLengthValue::Literal(3, CssUnit::kNumber);
      return;
  }
}

}  // namespace mitto
