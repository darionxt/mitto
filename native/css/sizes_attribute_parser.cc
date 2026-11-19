#include "mitto/css/sizes_attribute_parser.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "base/numerics/safe_conversions.h"
#include "base/strings/string_util.h"
#include "mitto/css/css_token.h"
#include "mitto/css/css_token_stream.h"
#include "mitto/css/media_query.h"
#include "mitto/css/media_query_evaluator.h"

namespace mitto {
namespace {

// CSSMathOperator, of those a sizes attribute's math takes.
enum class Operator {
  kInvalid,
  kAdd,
  kSubtract,
  kMultiply,
  kDivide,
  kMin,
  kMax,
};

// A value or an operator of the expression in reverse Polish notation.
struct MathValue {
  double value = 0;
  bool is_length = false;
  Operator operation = Operator::kInvalid;
};

// What the shunting yard keeps on its stack in place of Blink's tokens.
struct StackEntry {
  enum class Kind {
    kFunction,
    kLeftParenthesis,
    kComma,
    kOperator,
  };
  Kind kind;
  // For a function, its name in lower case.
  std::u16string name;
  Operator operation = Operator::kInvalid;
};

bool AtIdent(const CssToken& token, std::u16string_view ident) {
  return token.type == CssTokenType::kIdent &&
         base::EqualsCaseInsensitiveASCII(token.value, ident);
}

// ParseCSSArithmeticOperator().
Operator ArithmeticOperator(const CssToken& token) {
  if (token.type != CssTokenType::kDelimiter) {
    return Operator::kInvalid;
  }
  switch (token.delimiter) {
    case '+':
      return Operator::kAdd;
    case '-':
      return Operator::kSubtract;
    case '*':
      return Operator::kMultiply;
    case '/':
      return Operator::kDivide;
    default:
      return Operator::kInvalid;
  }
}

bool IsValidMathFunction(std::u16string_view name) {
  return base::EqualsCaseInsensitiveASCII(name, u"min") ||
         base::EqualsCaseInsensitiveASCII(name, u"max") ||
         base::EqualsCaseInsensitiveASCII(name, u"clamp") ||
         base::EqualsCaseInsensitiveASCII(name, u"calc");
}

// Blink's SizesMathFunctionParser: calc(), min(), max() and clamp() of
// lengths and numbers, through the shunting yard algorithm.
class SizesMathFunctionParser {
 public:
  SizesMathFunctionParser(CssTokenStream& stream,
                          const MediaValues& media_values)
      : media_values_(media_values) {
    is_valid_ = CalcToReversePolishNotation(stream) && Calculate();
  }

  bool IsValid() const { return is_valid_; }
  float Result() const { return result_; }

 private:
  static bool OperatorPriority(Operator op, bool& high_priority) {
    if (op == Operator::kAdd || op == Operator::kSubtract) {
      high_priority = false;
    } else if (op == Operator::kMultiply || op == Operator::kDivide) {
      high_priority = true;
    } else {
      return false;
    }
    return true;
  }

  bool HandleOperator(std::vector<StackEntry>& stack, Operator incoming) {
    bool incoming_priority;
    if (!OperatorPriority(incoming, incoming_priority)) {
      return false;
    }
    while (!stack.empty()) {
      const StackEntry& top = stack.back();
      if (top.kind != StackEntry::Kind::kOperator) {
        break;
      }
      bool stack_priority;
      if (!OperatorPriority(top.operation, stack_priority)) {
        return false;
      }
      if (incoming_priority && !stack_priority) {
        break;
      }
      value_list_.push_back({.operation = top.operation});
      stack.pop_back();
    }
    stack.push_back(
        {.kind = StackEntry::Kind::kOperator, .operation = incoming});
    return true;
  }

  bool HandleRightParenthesis(std::vector<StackEntry>& stack) {
    size_t comma_count = 0;
    while (!stack.empty() &&
           stack.back().kind != StackEntry::Kind::kLeftParenthesis &&
           stack.back().kind != StackEntry::Kind::kFunction) {
      if (stack.back().kind == StackEntry::Kind::kComma) {
        ++comma_count;
      } else {
        value_list_.push_back({.operation = stack.back().operation});
      }
      stack.pop_back();
    }
    if (stack.empty()) {
      return false;
    }
    const StackEntry left_side = std::move(stack.back());
    stack.pop_back();
    if (left_side.kind == StackEntry::Kind::kLeftParenthesis ||
        left_side.name == u"calc") {
      return !comma_count;
    }
    if (left_side.name == u"clamp") {
      if (comma_count != 2) {
        return false;
      }
      // clamp(MIN, VAL, MAX) is max(MIN, min(VAL, MAX)).
      value_list_.push_back({.operation = Operator::kMin});
      value_list_.push_back({.operation = Operator::kMax});
      return true;
    }
    const Operator op =
        left_side.name == u"min" ? Operator::kMin : Operator::kMax;
    for (size_t i = 0; i < comma_count; ++i) {
      value_list_.push_back({.operation = op});
    }
    return true;
  }

  bool HandleComma(std::vector<StackEntry>& stack) {
    while (!stack.empty() && stack.back().kind != StackEntry::Kind::kFunction &&
           stack.back().kind != StackEntry::Kind::kLeftParenthesis &&
           stack.back().kind != StackEntry::Kind::kComma) {
      value_list_.push_back({.operation = stack.back().operation});
      stack.pop_back();
    }
    if (stack.empty() ||
        stack.back().kind == StackEntry::Kind::kLeftParenthesis) {
      return false;
    }
    stack.push_back({.kind = StackEntry::Kind::kComma});
    return true;
  }

  bool CalcToReversePolishNotation(CssTokenStream& stream) {
    const CssTokenStream::State savepoint = stream.Save();
    std::vector<StackEntry> stack;
    if (!ConsumeCalc(stream, stack)) {
      stream.Restore(savepoint);
      return false;
    }
    while (!stack.empty()) {
      const StackEntry::Kind kind = stack.back().kind;
      if (kind != StackEntry::Kind::kLeftParenthesis &&
          kind != StackEntry::Kind::kFunction) {
        value_list_.push_back({.operation = stack.back().operation});
      }
      stack.pop_back();
    }
    return true;
  }

  // Does not restore the stream on failure.
  bool ConsumeCalc(CssTokenStream& stream, std::vector<StackEntry>& stack) {
    if (stream.Peek().type != CssTokenType::kFunction ||
        !IsValidMathFunction(stream.Peek().value)) {
      return false;
    }
    stack.push_back({.kind = StackEntry::Kind::kFunction,
                     .name = base::ToLowerASCII(stream.Peek().value)});
    {
      const CssTokenStream::BlockGuard guard(stream);
      if (!ConsumeBlockContent(stream, stack)) {
        return false;
      }
    }
    if (!HandleRightParenthesis(stack)) {
      return false;
    }
    stream.ConsumeWhitespace();
    return true;
  }

  bool ConsumeBlockContent(CssTokenStream& stream,
                           std::vector<StackEntry>& stack) {
    while (!stream.AtEnd()) {
      const CssToken& token = stream.Peek();
      switch (token.type) {
        case CssTokenType::kNumber:
          value_list_.push_back({.value = token.numeric_value});
          stream.Consume();
          break;
        case CssTokenType::kDimension: {
          const CssToken consumed = stream.Consume();
          if (!IsLengthUnit(consumed.unit)) {
            return false;
          }
          value_list_.push_back(
              {.value = media_values_->LengthResolver().ComputedPixels(
                   consumed.numeric_value, consumed.unit),
               .is_length = true});
          break;
        }
        case CssTokenType::kDelimiter: {
          const Operator op = ArithmeticOperator(stream.Consume());
          if (!HandleOperator(stack, op)) {
            return false;
          }
          break;
        }
        case CssTokenType::kFunction:
          if (!IsValidMathFunction(token.value)) {
            return false;
          }
          stack.push_back({.kind = StackEntry::Kind::kFunction,
                           .name = base::ToLowerASCII(token.value)});
          if (!ConsumeBlock(stream, stack)) {
            return false;
          }
          break;
        case CssTokenType::kLeftParenthesis:
          stack.push_back({.kind = StackEntry::Kind::kLeftParenthesis});
          if (!ConsumeBlock(stream, stack)) {
            return false;
          }
          break;
        case CssTokenType::kComma:
          stream.Consume();
          if (!HandleComma(stack)) {
            return false;
          }
          break;
        case CssTokenType::kWhitespace:
          stream.Consume();
          break;
        default:
          return false;
      }
    }
    return true;
  }

  bool ConsumeBlock(CssTokenStream& stream, std::vector<StackEntry>& stack) {
    {
      const CssTokenStream::BlockGuard guard(stream);
      if (!ConsumeBlockContent(stream, stack)) {
        return false;
      }
    }
    return HandleRightParenthesis(stack);
  }

  static bool OperateOnStack(std::vector<MathValue>& stack, Operator op) {
    if (stack.size() < 2) {
      return false;
    }
    const MathValue right = stack.back();
    stack.pop_back();
    const MathValue left = stack.back();
    stack.pop_back();
    switch (op) {
      case Operator::kAdd:
      case Operator::kSubtract:
      case Operator::kMin:
      case Operator::kMax: {
        if (right.is_length != left.is_length) {
          return false;
        }
        double value = 0;
        if (op == Operator::kAdd) {
          value = left.value + right.value;
        } else if (op == Operator::kSubtract) {
          value = left.value - right.value;
        } else if (op == Operator::kMin) {
          value = std::min(left.value, right.value);
        } else {
          value = std::max(left.value, right.value);
        }
        stack.push_back({.value = value, .is_length = left.is_length});
        return true;
      }
      case Operator::kMultiply:
        if (right.is_length && left.is_length) {
          return false;
        }
        stack.push_back({.value = left.value * right.value,
                         .is_length = right.is_length || left.is_length});
        return true;
      case Operator::kDivide:
        if (right.is_length || right.value == 0) {
          return false;
        }
        stack.push_back(
            {.value = left.value / right.value, .is_length = left.is_length});
        return true;
      case Operator::kInvalid:
        return false;
    }
  }

  bool Calculate() {
    std::vector<MathValue> stack;
    for (const MathValue& value : value_list_) {
      if (value.operation == Operator::kInvalid) {
        stack.push_back(value);
      } else if (!OperateOnStack(stack, value.operation)) {
        return false;
      }
    }
    if (stack.size() == 1 && stack.back().is_length) {
      result_ = std::max(base::saturated_cast<float>(stack.back().value), 0.0f);
      return true;
    }
    return false;
  }

  const raw_ref<const MediaValues> media_values_;
  std::vector<MathValue> value_list_;
  float result_ = 0;
  bool is_valid_ = false;
};

}  // namespace

SizesAttributeParser::SizesAttributeParser(
    const MediaValues& media_values,
    const MediaQueryFeatures& features,
    std::optional<std::u16string_view> attribute,
    const SizesAutoContext& auto_context)
    : media_values_(media_values),
      features_(features),
      auto_context_(auto_context) {
  CssTokenStream stream(attribute.value_or(std::u16string_view()));
  is_valid_ = Parse(stream);
}

SizesAttributeParser::~SizesAttributeParser() = default;

bool SizesAttributeParser::Parse(CssTokenStream& stream) {
  while (!stream.AtEnd()) {
    stream.ConsumeWhitespace();
    if (AtIdent(stream.Peek(), u"auto")) {
      // Fallback sizes may follow auto, for agents that do not know it.
      is_auto_ = true;
      stream.Consume();
      stream.ConsumeWhitespace();
      if (stream.AtEnd()) {
        return true;
      }
      if (stream.Peek().type != CssTokenType::kComma) {
        return false;
      }
      stream.Consume();
      continue;
    }
    const CssTokenStream::State savepoint = stream.Save();
    const std::unique_ptr<MediaQuerySet> media_condition =
        ParseMediaCondition(stream, *features_);
    if (!media_condition ||
        !MediaQueryEvaluator(*media_values_).Eval(*media_condition)) {
      // A calc() reads as <general-enclosed> and fails as a condition, where
      // it is a size, so the stream goes back.
      stream.Restore(savepoint);
    }
    if (stream.Peek().type != CssTokenType::kComma) {
      float length;
      if (CalculateLengthInPixels(stream, length)) {
        stream.ConsumeWhitespace();
        if (stream.AtEnd() || stream.Peek().type == CssTokenType::kComma) {
          size_ = length;
          size_was_set_ = true;
          return true;
        }
      }
    }
    stream.SkipUntilPeekedTypeIs(CssTokenType::kComma);
    if (!stream.AtEnd()) {
      stream.Consume();
    }
  }
  return false;
}

bool SizesAttributeParser::CalculateLengthInPixels(CssTokenStream& stream,
                                                   float& result) const {
  const CssToken& start_token = stream.Peek();
  switch (start_token.type) {
    case CssTokenType::kDimension: {
      if (!IsLengthUnit(start_token.unit)) {
        return false;
      }
      const double length = media_values_->LengthResolver().ComputedPixels(
          start_token.numeric_value, start_token.unit);
      if (length < 0) {
        return false;
      }
      result = base::saturated_cast<float>(length);
      stream.Consume();
      return true;
    }
    case CssTokenType::kFunction: {
      const SizesMathFunctionParser calc_parser(stream, *media_values_);
      if (!calc_parser.IsValid()) {
        return false;
      }
      result = calc_parser.Result();
      return true;
    }
    case CssTokenType::kNumber:
      if (start_token.numeric_value) {
        return false;
      }
      stream.Consume();
      result = 0;
      return true;
    default:
      return false;
  }
}

float SizesAttributeParser::Size() const {
  return is_valid_ ? EffectiveSize() : EffectiveSizeDefaultValue();
}

float SizesAttributeParser::EffectiveSize() const {
  // https://html.spec.whatwg.org/#parsing-a-sizes-attribute, 3.3: auto, for
  // an img that allows auto-sizes, is its layout box's width while it is
  // rendered, and its last auto-sizes width otherwise.
  if (is_auto_ && auto_context_.allows_auto_sizes) {
    if (auto_context_.rendered_width) {
      return *auto_context_.rendered_width;
    }
    if (auto_context_.last_auto_sizes_width) {
      return *auto_context_.last_auto_sizes_width;
    }
  }
  if (size_was_set_) {
    return size_;
  }
  return EffectiveSizeDefaultValue();
}

float SizesAttributeParser::EffectiveSizeDefaultValue() const {
  // 100vw.
  return base::saturated_cast<float>(media_values_->viewport_width);
}

}  // namespace mitto
