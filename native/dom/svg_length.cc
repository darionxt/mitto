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

// SVGViewportResolver: one size of the viewport that a percentage of
// `element`'s takes, from the nearest svg element above it: its viewBox's,
// or else its width or height, which needs only the same size of the svg
// element's own viewport. An outermost svg element's size is its layout
// box's in Blink, which the window stands for here.
float ViewportSize(const SVGElement& element, bool width) {
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
        return width ? (*numbers)[2] : (*numbers)[3];
      }
    }
    const std::u16string* text =
        svg.FindAttribute(width ? u"width" : u"height");
    const std::optional<SVGLengthValue> value =
        text ? SVGLengthValue::Parse(*text) : std::nullopt;
    return ResolveToUserUnits(
        value.value_or(SVGLengthValue::Literal(100, CssUnit::kPercentage)),
        &svg, width ? SVGLengthMode::kWidth : SVGLengthMode::kHeight);
  }
  const CssLengthResolver resolver = ResolverFor(&element);
  return static_cast<float>(width ? resolver.viewport_width
                                  : resolver.viewport_height);
}

float ViewportDimension(const SVGElement& element, SVGLengthMode mode) {
  switch (mode) {
    case SVGLengthMode::kWidth:
      return ViewportSize(element, /*width=*/true);
    case SVGLengthMode::kHeight:
      return ViewportSize(element, /*width=*/false);
    case SVGLengthMode::kOther: {
      const float width = ViewportSize(element, /*width=*/true);
      const float height = ViewportSize(element, /*width=*/false);
      return std::sqrt((width * width + height * height) / 2);
    }
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

// Blink's CSSVariableParser, for a value with a reference to substitute, as
// an SVG length takes text with var(): its tokens from `begin` to `end`.
class UnparsedValueParser {
 public:
  explicit UnparsedValueParser(std::u16string_view text)
      : tokens_(TokenizeCss(text)) {
    while (!tokens_.empty() &&
           tokens_.back().type == CssTokenType::kEndOfFile) {
      tokens_.pop_back();
    }
  }

  // ConsumeUnparsedDeclaration() with a reference that it must hold, a
  // restricted value and no !important.
  bool IsValidDeclaration() {
    bool has_reference = false;
    return ConsumeValue(SkipWhitespace(0, tokens_.size()), tokens_.size(),
                        /*restricted=*/true, has_reference) &&
           has_reference;
  }

 private:
  // Where the block that starts at `index` ends, or the end of the input,
  // which closes it.
  size_t BlockEnd(size_t index, size_t end) const {
    int depth = 0;
    for (size_t i = index; i < end; ++i) {
      if (tokens_[i].block_type == CssBlockType::kBlockStart) {
        ++depth;
      } else if (tokens_[i].block_type == CssBlockType::kBlockEnd &&
                 --depth == 0) {
        return i;
      }
    }
    return end;
  }

  size_t SkipWhitespace(size_t index, size_t end) const {
    while (index < end && tokens_[index].type == CssTokenType::kWhitespace) {
      ++index;
    }
    return index;
  }

  // ConsumeUnparsedValue(): no bad token or stray closing bracket, no
  // semicolon or ! outside a block, and, when `restricted`, a {} block only
  // as the whole value.
  bool ConsumeValue(size_t begin,
                    size_t end,
                    bool restricted,
                    bool& has_reference) const {
    int depth = 0;
    size_t top_level_values = 0;
    bool has_top_level_brace = false;
    for (size_t i = begin; i < end; ++i) {
      const CssToken& token = tokens_[i];
      const bool at_top_level = depth == 0;
      if (token.type == CssTokenType::kFunction) {
        if (std::optional<bool> valid =
                ConsumeSubstitution(token.value, i, end, has_reference)) {
          if (!*valid) {
            return false;
          }
          i = BlockEnd(i, end);
          continue;
        }
      }
      if (token.block_type == CssBlockType::kBlockStart) {
        ++depth;
      } else if (token.block_type == CssBlockType::kBlockEnd) {
        if (depth == 0) {
          return false;
        }
        --depth;
      } else {
        switch (token.type) {
          case CssTokenType::kBadString:
          case CssTokenType::kBadUrl:
          // A closing bracket that closes no block.
          case CssTokenType::kRightParenthesis:
          case CssTokenType::kRightBracket:
          case CssTokenType::kRightBrace:
            return false;
          case CssTokenType::kSemicolon:
            if (depth == 0) {
              return false;
            }
            break;
          case CssTokenType::kDelimiter:
            if (token.delimiter == '!' && depth == 0) {
              return false;
            }
            break;
          default:
            break;
        }
      }
      // Blink counts whitespace as a value of its own here.
      if (restricted && at_top_level) {
        ++top_level_values;
        if (token.type == CssTokenType::kLeftBrace) {
          has_top_level_brace = true;
        }
        if (has_top_level_brace && top_level_values > 1) {
          return false;
        }
      }
    }
    return true;
  }

  // A substitution function at `index`: whether it is valid, or nothing for
  // a function that substitutes nothing.
  std::optional<bool> ConsumeSubstitution(std::u16string_view name,
                                          size_t index,
                                          size_t end,
                                          bool& has_reference) const {
    const size_t close = BlockEnd(index, end);
    const size_t begin = index + 1;
    if (name.size() > 2 && name.substr(0, 2) == u"--") {
      // An author's function, whose arguments are values.
      has_reference = true;
      return ConsumeValue(begin, close, /*restricted=*/false, has_reference);
    }
    const std::u16string lower = base::ToLowerASCII(name);
    if (lower == u"inherit") {
      // Behind CSSInheritFunction, which Chrome 157 has off.
      return false;
    }
    if (lower == u"var") {
      has_reference = true;
      return ConsumeVariableReference(begin, close, has_reference);
    }
    if (lower == u"env") {
      has_reference = true;
      return ConsumeEnvReference(begin, close, has_reference);
    }
    if (lower == u"attr") {
      has_reference = true;
      return ConsumeAttributeReference(begin, close, has_reference);
    }
    if (lower == u"if") {
      has_reference = true;
      return ConsumeIf(begin, close, has_reference);
    }
    return std::nullopt;
  }

  // The fallback after a comma, or nothing.
  bool ConsumeFallback(size_t index, size_t end, bool& has_reference) const {
    if (index == end) {
      return true;
    }
    if (tokens_[index].type != CssTokenType::kComma) {
      return false;
    }
    return ConsumeValue(index + 1, end, /*restricted=*/false, has_reference);
  }

  // ConsumeVariableReference(): a custom property's name, of "--" and one
  // character at least, and a fallback.
  bool ConsumeVariableReference(size_t begin,
                                size_t end,
                                bool& has_reference) const {
    size_t i = SkipWhitespace(begin, end);
    if (i == end || tokens_[i].type != CssTokenType::kIdent ||
        tokens_[i].value.size() < 3 || tokens_[i].value.substr(0, 2) != u"--") {
      return false;
    }
    return ConsumeFallback(SkipWhitespace(i + 1, end), end, has_reference);
  }

  // ConsumeEnvVariableReference(): a name, the indices of a variable of more
  // dimensions, and a fallback.
  bool ConsumeEnvReference(size_t begin,
                           size_t end,
                           bool& has_reference) const {
    size_t i = SkipWhitespace(begin, end);
    if (i == end || tokens_[i].type != CssTokenType::kIdent) {
      return false;
    }
    i = SkipWhitespace(i + 1, end);
    while (i < end && tokens_[i].type == CssTokenType::kNumber) {
      if (tokens_[i].numeric_value_type != CssNumericValueType::kInteger ||
          tokens_[i].numeric_value < 0) {
        return false;
      }
      i = SkipWhitespace(i + 1, end);
    }
    return ConsumeFallback(i, end, has_reference);
  }

  // ConsumeAttributeReference(): a name, a type or unit, and a fallback.
  bool ConsumeAttributeReference(size_t begin,
                                 size_t end,
                                 bool& has_reference) const {
    size_t i = SkipWhitespace(begin, end);
    if (i == end || tokens_[i].type != CssTokenType::kIdent) {
      return false;
    }
    i = SkipWhitespace(i + 1, end);
    if (i < end && tokens_[i].type == CssTokenType::kIdent) {
      i = SkipWhitespace(i + 1, end);
    } else if (i < end && tokens_[i].type == CssTokenType::kFunction) {
      i = SkipWhitespace(BlockEnd(i, end) + 1, end);
    }
    return ConsumeFallback(std::min(i, end), end, has_reference);
  }

  // ConsumeIf(): branches that semicolons part, each a condition, a colon
  // and a value.
  bool ConsumeIf(size_t begin, size_t end, bool& has_reference) const {
    size_t i = begin;
    bool any_branch = false;
    while (true) {
      i = SkipWhitespace(i, end);
      if (i == end) {
        return any_branch;
      }
      // The condition: else, or a style(), media() or supports() query that
      // not, and and or may join.
      bool has_condition = false;
      while (i < end && tokens_[i].type != CssTokenType::kColon) {
        const CssToken& token = tokens_[i];
        if (token.type == CssTokenType::kFunction ||
            token.type == CssTokenType::kLeftParenthesis) {
          i = BlockEnd(i, end);
          has_condition = true;
        } else if (token.type == CssTokenType::kIdent) {
          has_condition = has_condition || base::EqualsCaseInsensitiveASCII(
                                               token.value, u"else");
        } else if (token.type != CssTokenType::kWhitespace) {
          return false;
        }
        ++i;
      }
      if (!has_condition || i >= end) {
        return false;
      }
      size_t value_end = i + 1;
      int depth = 0;
      for (; value_end < end; ++value_end) {
        const CssToken& token = tokens_[value_end];
        if (token.block_type == CssBlockType::kBlockStart) {
          ++depth;
        } else if (token.block_type == CssBlockType::kBlockEnd) {
          --depth;
        } else if (token.type == CssTokenType::kSemicolon && depth == 0) {
          break;
        }
      }
      if (!ConsumeValue(i + 1, value_end, /*restricted=*/false,
                        has_reference)) {
        return false;
      }
      any_branch = true;
      i = value_end == end ? end : value_end + 1;
    }
  }

  std::vector<CssToken> tokens_;
};

bool IsUnparsedDeclarationWithVariable(std::u16string_view text) {
  return UnparsedValueParser(text).IsValidDeclaration();
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
SVGLength* SVGLength::ForItem(SVGListItem<SVGLengthValue>& item,
                              bool read_only,
                              SVGAnimatedListBase* binding) {
  return bindings::MakeGarbageCollected<SVGLength>(item, read_only, binding);
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
      item_(owner ? nullptr
                  : bindings::MakeGarbageCollected<SVGListItem<SVGLengthValue>>(
                        SVGLengthValue::Literal(0, CssUnit::kNumber))) {}

SVGLength::SVGLength(SVGListItem<SVGLengthValue>& item,
                     bool read_only,
                     SVGAnimatedListBase* binding)
    : is_anim_val_(read_only), item_(&item), binding_(binding) {}

SVGLength::~SVGLength() = default;

const bindings::WrapperTypeInfo* SVGLength::GetWrapperTypeInfo() const {
  return &bindings::V8SVGLength::kWrapperTypeInfo;
}

void SVGLength::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(owner_);
  visitor->Trace(item_);
  visitor->Trace(binding_);
}

SVGListItem<SVGLengthValue>* SVGLength::ItemToInsert(
    SVGAnimatedListBase& list) {
  if (!owner_ && !is_anim_val_ && !binding_ && !item_->owner) {
    binding_ = &list;
    return item_.Get();
  }
  return bindings::MakeGarbageCollected<SVGListItem<SVGLengthValue>>(Target());
}

SVGLengthValue SVGLength::Target() const {
  return owner_ ? owner_->BaseValue() : item_->value;
}

void SVGLength::Commit(const SVGLengthValue& value) {
  if (owner_) {
    owner_->SetBaseValue(value);
    return;
  }
  item_->value = value;
  if (item_->owner) {
    item_->owner->ItemChanged(item_.Get());
  }
}

SVGElement* SVGLength::ContextElement() const {
  if (owner_) {
    return &owner_->Element();
  }
  return binding_ ? &binding_->Element() : nullptr;
}

SVGLengthMode SVGLength::Mode() const {
  if (owner_) {
    return owner_->Mode();
  }
  return binding_ ? binding_->LengthMode() : SVGLengthMode::kOther;
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
