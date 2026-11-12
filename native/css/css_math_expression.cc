#include "mitto/css/css_math_expression.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/css/css_serialization.h"
#include "mitto/css/css_token_stream.h"

namespace mitto {

namespace {

using Node = CssMathNode;
using NodePtr = std::shared_ptr<CssMathNode>;
using Operands = std::vector<NodePtr>;

// How deep an expression may nest, as in Blink.
constexpr int kMaxExpressionDepth = 100;

bool EqualsIgnoringAsciiCase(std::u16string_view a, std::string_view b) {
  return base::EqualsCaseInsensitiveASCII(a, base::ASCIIToUTF16(b));
}

bool IsNumericLiteral(const Node& node) {
  return node.kind == Node::Kind::kLiteral;
}

bool IsOperation(const Node& node) {
  return node.kind == Node::Kind::kOperation;
}

bool IsInvert(const Node& node) {
  return IsOperation(node) && node.op == CssMathOperator::kInvert;
}

bool IsAddOrSubtract(const Node& node) {
  return IsOperation(node) && (node.op == CssMathOperator::kAdd ||
                               node.op == CssMathOperator::kSubtract);
}

bool IsMultiplyOrDivide(const Node& node) {
  return IsOperation(node) && (node.op == CssMathOperator::kMultiply ||
                               node.op == CssMathOperator::kDivide);
}

bool IsComparison(const Node& node) {
  return IsOperation(node) && (node.op == CssMathOperator::kMin ||
                               node.op == CssMathOperator::kMax ||
                               node.op == CssMathOperator::kClamp);
}

bool IsNone(const Node& node) {
  return node.kind == Node::Kind::kNone;
}

bool AllOperandsAreNumeric(const Node& node) {
  return std::all_of(
      node.operands.begin(), node.operands.end(),
      [](const NodePtr& operand) { return IsNumericLiteral(*operand); });
}

NodePtr Copy(const Node& node) {
  return std::make_shared<Node>(node);
}

// As Blink's
// CSSMathExpressionNumericLiteral::CSSMathExpressionNumericLiteral().
NodePtr CreateLiteral(double value, CssUnit unit) {
  auto node = std::make_shared<Node>();
  node->kind = Node::Kind::kLiteral;
  node->category = CssCalcCategoryOf(unit);
  node->value = value;
  node->unit = unit;
  // "If root is a dimension that is not expressed in its canonical unit, and
  // there is enough information available to convert it to the canonical
  // unit, do so." Numbers stay as they are.
  if (node->category != CssCalcCategory::kNumber &&
      !IsRelativeLengthUnit(unit)) {
    const CssUnit canonical = CanonicalUnitFor(CssUnitCategoryOf(unit));
    if (canonical != CssUnit::kUnknown) {
      node->value = value * ConversionToCanonicalUnits(unit);
      node->unit = canonical;
    }
  }
  return node;
}

NodePtr CreateNone() {
  auto node = std::make_shared<Node>();
  node->kind = Node::Kind::kNone;
  node->category = CssCalcCategory::kOther;
  return node;
}

NodePtr CreateOperation(CssMathOperator op,
                        Operands operands,
                        CssCalcCategory category) {
  auto node = std::make_shared<Node>();
  node->kind = Node::Kind::kOperation;
  node->op = op;
  node->operands = std::move(operands);
  node->category = category;
  return node;
}

// As Blink's CSSMathExpressionOperation::EvaluateOperator().
double EvaluateOperator(const std::vector<double>& operands,
                        CssMathOperator op) {
  for (double operand : operands) {
    if (std::isnan(operand)) {
      return operand;
    }
  }
  switch (op) {
    case CssMathOperator::kAdd:
      return operands[0] + operands[1];
    case CssMathOperator::kSubtract:
      return operands[0] - operands[1];
    case CssMathOperator::kMultiply:
      return operands[0] * operands[1];
    case CssMathOperator::kDivide:
      return operands[0] / operands[1];
    case CssMathOperator::kInvert:
      return 1.0 / operands[0];
    case CssMathOperator::kMin: {
      double minimum = operands[0];
      for (double operand : operands) {
        if (minimum == 0 && operand == 0 &&
            std::signbit(minimum) != std::signbit(operand)) {
          minimum = -0.0;
          continue;
        }
        minimum = std::min(minimum, operand);
      }
      return minimum;
    }
    case CssMathOperator::kMax: {
      double maximum = operands[0];
      for (double operand : operands) {
        if (maximum == 0 && operand == 0 &&
            std::signbit(maximum) != std::signbit(operand)) {
          maximum = 0.0;
          continue;
        }
        maximum = std::max(maximum, operand);
      }
      return maximum;
    }
    case CssMathOperator::kClamp: {
      const double min = operands[0];
      const double val = operands[1];
      const double max = operands[2];
      // clamp(MIN, VAL, MAX) is max(MIN, min(VAL, MAX)).
      return EvaluateOperator(
          {min, EvaluateOperator({val, max}, CssMathOperator::kMin)},
          CssMathOperator::kMax);
    }
  }
  return 0;
}

// The value of a node that needs nothing resolved, in its canonical unit.
std::optional<double> ValueInCanonicalUnit(const Node& node,
                                           const CssLengthResolver* resolver) {
  switch (node.kind) {
    case Node::Kind::kNone:
      return std::nullopt;
    case Node::Kind::kLiteral:
      if (IsRelativeLengthUnit(node.unit)) {
        if (!resolver) {
          return std::nullopt;
        }
        return resolver->ComputedPixels(node.value, node.unit);
      }
      return node.value * ConversionToCanonicalUnits(node.unit);
    case Node::Kind::kOperation: {
      std::vector<double> values;
      values.reserve(node.operands.size());
      for (const NodePtr& operand : node.operands) {
        const std::optional<double> value =
            ValueInCanonicalUnit(*operand, resolver);
        if (!value) {
          return std::nullopt;
        }
        values.push_back(*value);
      }
      return EvaluateOperator(values, node.op);
    }
  }
  return std::nullopt;
}

// As Blink's CanEagerlySimplify().
bool CanEagerlySimplify(const Node& operand) {
  if (!IsNumericLiteral(operand)) {
    return false;
  }
  switch (operand.category) {
    case CssCalcCategory::kNumber:
    case CssCalcCategory::kAngle:
    case CssCalcCategory::kTime:
    case CssCalcCategory::kFrequency:
    case CssCalcCategory::kResolution:
      return true;
    case CssCalcCategory::kLength:
      return !IsRelativeLengthUnit(operand.unit);
    case CssCalcCategory::kOther:
      return false;
  }
  return false;
}

// What an arithmetic operation gives, or kOther where Blink's typed
// arithmetic would take it and this does not.
CssCalcCategory ArithmeticCategory(const Node& left,
                                   const Node& right,
                                   CssMathOperator op) {
  switch (op) {
    case CssMathOperator::kAdd:
    case CssMathOperator::kSubtract:
      return left.category == right.category ? left.category
                                             : CssCalcCategory::kOther;
    case CssMathOperator::kMultiply:
      if (left.category == CssCalcCategory::kNumber) {
        return right.category;
      }
      if (right.category == CssCalcCategory::kNumber) {
        return left.category;
      }
      return CssCalcCategory::kOther;
    case CssMathOperator::kDivide:
      return right.category == CssCalcCategory::kNumber
                 ? left.category
                 : CssCalcCategory::kOther;
    default:
      return CssCalcCategory::kOther;
  }
}

// As Blink's CSSMathExpressionOperation::CreateInvertFunction().
NodePtr CreateInvertFunction(const NodePtr& operand) {
  if (!IsOperation(*operand) && operand->category == CssCalcCategory::kNumber) {
    const std::optional<double> value = ValueInCanonicalUnit(*operand, nullptr);
    if (value && *value != 0) {
      return CreateLiteral(1.0 / *value, CssUnit::kNumber);
    }
  }
  if (IsInvert(*operand)) {
    return Copy(*operand->operands[0]);
  }
  return CreateOperation(CssMathOperator::kInvert, {operand},
                         CssCalcCategory::kNumber);
}

// As Blink's CSSMathExpressionOperation::CreateArithmeticOperation().
NodePtr CreateArithmeticOperation(const NodePtr& left,
                                  const NodePtr& right,
                                  CssMathOperator op) {
  const CssCalcCategory category = ArithmeticCategory(*left, *right, op);
  if (category == CssCalcCategory::kOther) {
    return nullptr;
  }
  NodePtr right_side = right;
  if (op == CssMathOperator::kDivide) {
    NodePtr inverted = CreateInvertFunction(right);
    if (left->category == CssCalcCategory::kNumber && IsNumericLiteral(*left) &&
        left->value == 1.0) {
      return inverted;
    }
    op = CssMathOperator::kMultiply;
    right_side = std::move(inverted);
  }
  return CreateOperation(op, {left, right_side}, category);
}

NodePtr CreateArithmeticOperationSimplified(const NodePtr& left,
                                            const NodePtr& right,
                                            CssMathOperator op);

// As Blink's MaybeDistributeArithmeticOperation().
NodePtr MaybeDistributeArithmeticOperation(const NodePtr& left,
                                           const NodePtr& right,
                                           CssMathOperator op) {
  if (op != CssMathOperator::kMultiply && op != CssMathOperator::kDivide) {
    return nullptr;
  }
  // Case (Op1 + Op2) * Num.
  if (IsAddOrSubtract(*left) && AllOperandsAreNumeric(*left) &&
      IsNumericLiteral(*right) && right->category == CssCalcCategory::kNumber) {
    NodePtr new_left =
        CreateArithmeticOperationSimplified(left->operands.front(), right, op);
    NodePtr new_right =
        CreateArithmeticOperationSimplified(left->operands.back(), right, op);
    if (!new_left || !new_right) {
      return nullptr;
    }
    NodePtr operation =
        CreateArithmeticOperationSimplified(new_left, new_right, left->op);
    if (operation) {
      operation->is_nested_calc = true;
    }
    return operation;
  }
  // Case Num * (Op1 + Op2), but not Num / (Op1 + Op2).
  if (IsAddOrSubtract(*right) && AllOperandsAreNumeric(*right) &&
      IsNumericLiteral(*left) && left->category == CssCalcCategory::kNumber &&
      op != CssMathOperator::kDivide) {
    NodePtr new_right =
        CreateArithmeticOperationSimplified(left, right->operands.front(), op);
    NodePtr new_left =
        CreateArithmeticOperationSimplified(left, right->operands.back(), op);
    if (!new_left || !new_right) {
      return nullptr;
    }
    NodePtr operation =
        CreateArithmeticOperationSimplified(new_right, new_left, right->op);
    if (operation) {
      operation->is_nested_calc = true;
    }
    return operation;
  }
  return nullptr;
}

// As Blink's CanArithmeticOperationBeSimplified().
bool CanArithmeticOperationBeSimplified(const Node& left,
                                        const Node& right,
                                        CssMathOperator op) {
  if (!IsNumericLiteral(left) || !IsNumericLiteral(right)) {
    return false;
  }
  if ((op == CssMathOperator::kMultiply || op == CssMathOperator::kDivide) &&
      left.category != CssCalcCategory::kNumber &&
      right.category != CssCalcCategory::kNumber) {
    return false;
  }
  return true;
}

// As Blink's CSSMathExpressionOperation::CreateArithmeticOperationSimplified().
NodePtr CreateArithmeticOperationSimplified(const NodePtr& left,
                                            const NodePtr& right,
                                            CssMathOperator op) {
  // 'none' of clamp() stands only on its own.
  if (IsNone(*left) || IsNone(*right)) {
    return nullptr;
  }
  if (NodePtr result = MaybeDistributeArithmeticOperation(left, right, op)) {
    return result;
  }
  if (!CanArithmeticOperationBeSimplified(*left, *right, op)) {
    return CreateArithmeticOperation(left, right, op);
  }

  // Simplify numbers.
  if (left->category == CssCalcCategory::kNumber &&
      right->category == CssCalcCategory::kNumber) {
    return CreateLiteral(EvaluateOperator({left->value, right->value}, op),
                         CssUnit::kNumber);
  }

  if (op == CssMathOperator::kAdd || op == CssMathOperator::kSubtract) {
    // Simplify addition and subtraction between same types.
    if (left->category == right->category) {
      if (left->unit == right->unit) {
        return CreateLiteral(EvaluateOperator({left->value, right->value}, op),
                             left->unit);
      }
      const CssUnitCategory unit_category = CssUnitCategoryOf(left->unit);
      if (unit_category != CssUnitCategory::kOther &&
          unit_category == CssUnitCategoryOf(right->unit)) {
        const CssUnit canonical = CanonicalUnitFor(unit_category);
        if (canonical != CssUnit::kUnknown) {
          return CreateLiteral(
              EvaluateOperator(
                  {left->value * ConversionToCanonicalUnits(left->unit),
                   right->value * ConversionToCanonicalUnits(right->unit)},
                  op),
              canonical);
        }
      }
    }
  } else {
    // Simplify multiplying or dividing by a number.
    if (right->category != CssCalcCategory::kNumber &&
        op == CssMathOperator::kDivide) {
      return nullptr;
    }
    const NodePtr& number_side =
        left->category == CssCalcCategory::kNumber ? left : right;
    const NodePtr& other_side = number_side == left ? right : left;
    return CreateLiteral(
        EvaluateOperator({other_side->value, number_side->value}, op),
        other_side->unit);
  }
  return CreateArithmeticOperation(left, right, op);
}

struct NodeWithOperator {
  CssMathOperator op;
  NodePtr node;
};

// As Blink's MaybeNegateFirstNode().
NodePtr MaybeNegateFirstNode(CssMathOperator op, const NodePtr& node) {
  if (IsNumericLiteral(*node) && op == CssMathOperator::kSubtract) {
    return CreateLiteral(-node->value, node->unit);
  }
  return node;
}

// As Blink's MaybeChangeOperatorSignIfNesting().
CssMathOperator MaybeChangeOperatorSignIfNesting(bool is_in_nesting,
                                                 CssMathOperator outer_op,
                                                 CssMathOperator current_op) {
  if (is_in_nesting && outer_op == CssMathOperator::kSubtract &&
      current_op == CssMathOperator::kAdd) {
    return CssMathOperator::kSubtract;
  }
  if (is_in_nesting && outer_op == CssMathOperator::kSubtract &&
      current_op == CssMathOperator::kSubtract) {
    return CssMathOperator::kAdd;
  }
  return current_op;
}

// Calls `process_node` with each operand of the sum or product at `root`,
// flattened, and the operator in front of it.
// As Blink's TraverseNumericChildrenFromNode().
template <typename T>
void TraverseNumericChildrenFromNode(const NodePtr& root,
                                     CssMathOperator op,
                                     T&& process_node) {
  struct Pending {
    NodePtr node;
    CssMathOperator op;
    bool is_in_nesting;
  };
  std::vector<Pending> operation_nodes;
  const auto should_traverse = [](const Node& node, CssMathOperator op) {
    return op == CssMathOperator::kMultiply ? IsMultiplyOrDivide(node)
                                            : IsAddOrSubtract(node);
  };
  const auto push = [&](const NodePtr& node, CssMathOperator op,
                        bool is_in_nesting) {
    is_in_nesting |= node->is_nested_calc;
    const CssMathOperator back_op =
        MaybeChangeOperatorSignIfNesting(is_in_nesting, op, node->op);
    operation_nodes.push_back({node->operands.back(), back_op, is_in_nesting});
    operation_nodes.push_back({node->operands.front(), op, is_in_nesting});
  };
  if (!should_traverse(*root, op)) {
    process_node(root, op);
    return;
  }
  push(root, op, false);
  while (!operation_nodes.empty()) {
    Pending last = std::move(operation_nodes.back());
    operation_nodes.pop_back();
    if (!should_traverse(*last.node, last.op)) {
      process_node(last.node, last.op);
      continue;
    }
    push(last.node, last.op, last.is_in_nesting);
  }
}

// As Blink's MaybeSimplifySumOrProductNode().
NodePtr MaybeSimplifySumOrProductNode(const NodePtr& root) {
  const bool is_multiply = IsMultiplyOrDivide(*root);
  std::map<CssUnit, double> numeric_children;
  std::vector<NodeWithOperator> all_children;
  TraverseNumericChildrenFromNode(
      root, is_multiply ? CssMathOperator::kMultiply : CssMathOperator::kAdd,
      [&](const NodePtr& node, CssMathOperator op) {
        if (IsNumericLiteral(*node)) {
          const double value =
              op == CssMathOperator::kSubtract ? -node->value : node->value;
          auto it = numeric_children.find(node->unit);
          if (it != numeric_children.end()) {
            if (op == CssMathOperator::kMultiply) {
              it->second *= value;
            } else {
              it->second += value;
            }
          } else {
            numeric_children.emplace(node->unit, value);
          }
        }
        all_children.push_back({op, node});
      });

  double multiplicative_factor = 1.0;
  if (is_multiply) {
    auto it = numeric_children.find(CssUnit::kNumber);
    if (it != numeric_children.end()) {
      multiplicative_factor = it->second;
    }
  }

  std::set<CssUnit> used_units;
  NodePtr final_node;
  for (const NodeWithOperator& child : all_children) {
    if (is_multiply && IsNumericLiteral(*child.node) &&
        child.node->unit == CssUnit::kNumber) {
      continue;
    }
    CssMathOperator op = child.op;
    NodePtr node = child.node;
    // As Blink's MaybeReplaceNodeWithCombined().
    if (IsNumericLiteral(*node)) {
      auto it = numeric_children.find(node->unit);
      if (it != numeric_children.end()) {
        double value = it->second;
        if (is_multiply) {
          value *= multiplicative_factor;
          multiplicative_factor = 1.0;
        } else {
          op = std::signbit(value) ? CssMathOperator::kSubtract
                                   : CssMathOperator::kAdd;
          value = std::abs(value);
        }
        node = CreateLiteral(value, node->unit);
      }
      if (!used_units.insert(node->unit).second) {
        continue;
      }
    }
    if (!final_node) {
      final_node = Copy(*MaybeNegateFirstNode(op, node));
      continue;
    }
    final_node = CreateArithmeticOperationSimplified(final_node, node, op);
    if (!final_node) {
      return nullptr;
    }
  }

  if (is_multiply) {
    if (!final_node) {
      final_node = CreateLiteral(multiplicative_factor, CssUnit::kNumber);
    } else if (multiplicative_factor != 1.0) {
      final_node = CreateArithmeticOperationSimplified(
          final_node, CreateLiteral(multiplicative_factor, CssUnit::kNumber),
          CssMathOperator::kMultiply);
      if (!final_node) {
        return nullptr;
      }
    }
  }
  final_node->is_nested_calc = true;
  return final_node;
}

// As Blink's CSSMathExpressionOperation::CreateComparisonFunction().
NodePtr CreateComparisonFunction(Operands operands, CssMathOperator op) {
  if (op == CssMathOperator::kClamp) {
    const NodePtr& min = operands[0];
    const NodePtr& val = operands[1];
    const NodePtr& max = operands[2];
    if (IsNone(*val)) {
      return nullptr;
    }
    if (IsNone(*min) && IsNone(*max)) {
      return Copy(*val);
    }
    if (IsNone(*min)) {
      return CreateComparisonFunction({Copy(*val), Copy(*max)},
                                      CssMathOperator::kMin);
    }
    if (IsNone(*max)) {
      return CreateComparisonFunction({Copy(*min), Copy(*val)},
                                      CssMathOperator::kMax);
    }
  }

  const CssCalcCategory category = operands.front()->category;
  for (const NodePtr& operand : operands) {
    if (operand->category == CssCalcCategory::kOther ||
        operand->category != category) {
      return nullptr;
    }
  }

  if (std::all_of(operands.begin(), operands.end(), [](const NodePtr& node) {
        return CanEagerlySimplify(*node);
      })) {
    std::vector<double> values;
    values.reserve(operands.size());
    for (const NodePtr& operand : operands) {
      values.push_back(*ValueInCanonicalUnit(*operand, nullptr));
    }
    return CreateLiteral(
        EvaluateOperator(values, op),
        CanonicalUnitFor(CssUnitCategoryOf(operands.front()->unit)) ==
                CssUnit::kUnknown
            ? CssUnit::kNumber
            : CanonicalUnitFor(CssUnitCategoryOf(operands.front()->unit)));
  }

  if (operands.size() == 1) {
    return Copy(*operands.front());
  }
  return CreateOperation(op, std::move(operands), category);
}

std::optional<CssMathOperator> ArithmeticOperatorOf(const CssToken& token) {
  if (token.type != CssTokenType::kDelimiter) {
    return std::nullopt;
  }
  switch (token.delimiter) {
    case '+':
      return CssMathOperator::kAdd;
    case '-':
      return CssMathOperator::kSubtract;
    case '*':
      return CssMathOperator::kMultiply;
    case '/':
      return CssMathOperator::kDivide;
    default:
      return std::nullopt;
  }
}

// Upstream: CSSMathExpressionNodeParser
class Parser {
 public:
  struct State {
    int depth = 0;
    bool allow_clamp_none = false;
  };

  // `name` is the function's name, which the stream is inside.
  NodePtr ParseMathFunction(std::u16string_view name,
                            CssTokenStream& stream,
                            State state) {
    state.allow_clamp_none = false;
    size_t min_argument_count = 1;
    size_t max_argument_count = std::numeric_limits<size_t>::max();
    CssMathOperator op;
    bool is_calc = false;
    if (EqualsIgnoringAsciiCase(name, "calc") ||
        EqualsIgnoringAsciiCase(name, "-webkit-calc")) {
      max_argument_count = 1;
      is_calc = true;
      op = CssMathOperator::kAdd;
    } else if (EqualsIgnoringAsciiCase(name, "min")) {
      op = CssMathOperator::kMin;
    } else if (EqualsIgnoringAsciiCase(name, "max")) {
      op = CssMathOperator::kMax;
    } else if (EqualsIgnoringAsciiCase(name, "clamp")) {
      min_argument_count = 3;
      max_argument_count = 3;
      state.allow_clamp_none = true;
      op = CssMathOperator::kClamp;
    } else {
      // Blink's other math functions are Known Gaps.
      return nullptr;
    }

    Operands nodes;
    while (!stream.AtEnd() && nodes.size() < max_argument_count) {
      if (!nodes.empty()) {
        if (stream.Peek().type != CssTokenType::kComma) {
          return nullptr;
        }
        stream.ConsumeIncludingWhitespace();
      }
      stream.ConsumeWhitespace();
      NodePtr node = ParseValueExpression(stream, state);
      if (!node) {
        return nullptr;
      }
      nodes.push_back(std::move(node));
    }
    if (!stream.AtEnd() || nodes.size() < min_argument_count) {
      return nullptr;
    }
    if (is_calc) {
      return nodes.front();
    }
    return CreateComparisonFunction(std::move(nodes), op);
  }

 private:
  NodePtr ParseValue(CssTokenStream& stream,
                     State state,
                     bool& whitespace_after_token) {
    const CssToken token = stream.Consume();
    whitespace_after_token = stream.Peek().type == CssTokenType::kWhitespace;
    stream.ConsumeWhitespace();
    if (token.type == CssTokenType::kIdent) {
      if (EqualsIgnoringAsciiCase(token.value, "infinity")) {
        return CreateLiteral(std::numeric_limits<double>::infinity(),
                             CssUnit::kNumber);
      }
      if (EqualsIgnoringAsciiCase(token.value, "-infinity")) {
        return CreateLiteral(-std::numeric_limits<double>::infinity(),
                             CssUnit::kNumber);
      }
      if (EqualsIgnoringAsciiCase(token.value, "nan")) {
        return CreateLiteral(std::numeric_limits<double>::quiet_NaN(),
                             CssUnit::kNumber);
      }
      if (EqualsIgnoringAsciiCase(token.value, "pi")) {
        return CreateLiteral(std::numbers::pi, CssUnit::kNumber);
      }
      if (EqualsIgnoringAsciiCase(token.value, "e")) {
        return CreateLiteral(std::numbers::e, CssUnit::kNumber);
      }
      if (state.allow_clamp_none &&
          EqualsIgnoringAsciiCase(token.value, "none")) {
        return CreateNone();
      }
      return nullptr;
    }
    // Percentages are not allowed where a media query takes a math
    // function, as Blink's parsing flags have it there.
    if (token.type != CssTokenType::kNumber &&
        token.type != CssTokenType::kDimension) {
      return nullptr;
    }
    if (CssCalcCategoryOf(token.unit) == CssCalcCategory::kOther) {
      return nullptr;
    }
    return CreateLiteral(token.numeric_value, token.unit);
  }

  NodePtr ParseValueTerm(CssTokenStream& stream,
                         State state,
                         bool& whitespace_after_token) {
    if (stream.AtEnd()) {
      return nullptr;
    }
    const CssToken& next = stream.Peek();
    if (next.type == CssTokenType::kLeftParenthesis ||
        (next.type == CssTokenType::kFunction &&
         EqualsIgnoringAsciiCase(next.value, "calc"))) {
      NodePtr result;
      {
        CssTokenStream::BlockGuard guard(stream);
        stream.ConsumeWhitespace();
        result = ParseValueExpression(stream, state);
        if (!result || !stream.AtEnd() || IsNone(*result)) {
          return nullptr;
        }
        result->is_nested_calc = true;
      }
      whitespace_after_token = stream.Peek().type == CssTokenType::kWhitespace;
      stream.ConsumeWhitespace();
      return result;
    }
    if (next.type == CssTokenType::kFunction) {
      const std::u16string name = next.value;
      NodePtr result;
      {
        CssTokenStream::BlockGuard guard(stream);
        stream.ConsumeWhitespace();
        result = ParseMathFunction(name, stream, state);
      }
      whitespace_after_token = stream.Peek().type == CssTokenType::kWhitespace;
      stream.ConsumeWhitespace();
      return result;
    }
    if (next.block_type != CssBlockType::kNotBlock) {
      return nullptr;
    }
    return ParseValue(stream, state, whitespace_after_token);
  }

  NodePtr ParseValueMultiplicativeExpression(CssTokenStream& stream,
                                             State state,
                                             bool& whitespace_after_last) {
    if (stream.AtEnd()) {
      return nullptr;
    }
    NodePtr result = ParseValueTerm(stream, state, whitespace_after_last);
    if (!result) {
      return nullptr;
    }
    int result_depth = state.depth + 1;
    while (!stream.AtEnd()) {
      const std::optional<CssMathOperator> op =
          ArithmeticOperatorOf(stream.Peek());
      if (op != CssMathOperator::kMultiply && op != CssMathOperator::kDivide) {
        break;
      }
      stream.ConsumeIncludingWhitespace();
      NodePtr rhs = ParseValueTerm(stream, state, whitespace_after_last);
      if (!rhs) {
        return nullptr;
      }
      result = CreateArithmeticOperationSimplified(result, rhs, *op);
      if (!result) {
        return nullptr;
      }
      if (IsOperation(*result)) {
        if (++result_depth > kMaxExpressionDepth) {
          return nullptr;
        }
      } else {
        result_depth = state.depth + 1;
      }
    }
    if (IsMultiplyOrDivide(*result)) {
      result = MaybeSimplifySumOrProductNode(result);
    }
    return result;
  }

  NodePtr ParseAdditiveValueExpression(CssTokenStream& stream, State state) {
    if (stream.AtEnd()) {
      return nullptr;
    }
    bool whitespace_after_expr = false;
    NodePtr result = ParseValueMultiplicativeExpression(stream, state,
                                                        whitespace_after_expr);
    if (!result) {
      return nullptr;
    }
    int result_depth = state.depth + 1;
    while (!stream.AtEnd()) {
      const std::optional<CssMathOperator> op =
          ArithmeticOperatorOf(stream.Peek());
      if (op != CssMathOperator::kAdd && op != CssMathOperator::kSubtract) {
        break;
      }
      if (!whitespace_after_expr) {
        // calc(1px+ 2px) is invalid.
        return nullptr;
      }
      stream.Consume();
      if (stream.Peek().type != CssTokenType::kWhitespace) {
        // calc(1px +2px) is invalid.
        return nullptr;
      }
      stream.ConsumeIncludingWhitespace();
      NodePtr rhs = ParseValueMultiplicativeExpression(stream, state,
                                                       whitespace_after_expr);
      if (!rhs) {
        return nullptr;
      }
      result = CreateArithmeticOperationSimplified(result, rhs, *op);
      if (!result) {
        return nullptr;
      }
      if (IsOperation(*result)) {
        if (++result_depth > kMaxExpressionDepth) {
          return nullptr;
        }
      } else {
        result_depth = state.depth + 1;
      }
    }
    if (IsAddOrSubtract(*result)) {
      result = MaybeSimplifySumOrProductNode(result);
    }
    return result;
  }

  NodePtr ParseValueExpression(CssTokenStream& stream, State state) {
    if (++state.depth > kMaxExpressionDepth) {
      return nullptr;
    }
    return ParseAdditiveValueExpression(stream, state);
  }
};

std::u16string NodeCssText(const Node& node);

// As Blink's CollectSumOrProductInOrder().
std::vector<NodeWithOperator> CollectSumOrProductInOrder(const NodePtr& root) {
  std::map<CssUnit, std::vector<NodeWithOperator>> numeric_children;
  std::vector<NodeWithOperator> complex_children;
  TraverseNumericChildrenFromNode(
      root,
      IsAddOrSubtract(*root) ? CssMathOperator::kAdd
                             : CssMathOperator::kMultiply,
      [&](const NodePtr& node, CssMathOperator op) {
        if (IsNumericLiteral(*node)) {
          numeric_children[node->unit].push_back({op, node});
          return;
        }
        complex_children.push_back({op, node});
      });
  std::vector<NodeWithOperator> ret;
  const auto take = [&](CssUnit unit) {
    auto it = numeric_children.find(unit);
    if (it != numeric_children.end()) {
      ret.insert(ret.end(), it->second.begin(), it->second.end());
      numeric_children.erase(it);
    }
  };
  take(CssUnit::kNumber);
  take(CssUnit::kPercentage);
  std::vector<CssUnit> keys;
  for (const auto& [unit, nodes] : numeric_children) {
    keys.push_back(unit);
  }
  std::sort(keys.begin(), keys.end(), [](CssUnit a, CssUnit b) {
    return CssUnitName(a) < CssUnitName(b);
  });
  for (CssUnit unit : keys) {
    take(unit);
  }
  ret.insert(ret.end(), complex_children.begin(), complex_children.end());
  return ret;
}

std::u16string_view OperatorName(CssMathOperator op) {
  switch (op) {
    case CssMathOperator::kAdd:
      return u"+";
    case CssMathOperator::kSubtract:
      return u"-";
    case CssMathOperator::kMultiply:
      return u"*";
    case CssMathOperator::kDivide:
      return u"/";
    case CssMathOperator::kMin:
      return u"min";
    case CssMathOperator::kMax:
      return u"max";
    case CssMathOperator::kClamp:
      return u"clamp";
    case CssMathOperator::kInvert:
      return u"";
  }
  return u"";
}

// "If a result of this serialization starts with a "(" (open parenthesis)
// and ends with a ")" (close parenthesis), remove those characters from the
// result."
void SerializeTopLevelNode(const Node& node, std::u16string& result) {
  const std::u16string text = NodeCssText(node);
  if (text.starts_with(u'(')) {
    result.append(text, 1, text.size() - 2);
  } else {
    result += text;
  }
}

// As Blink's CSSMathExpressionOperation::CustomCSSText().
std::u16string NodeCssText(const Node& node) {
  switch (node.kind) {
    case Node::Kind::kNone:
      return u"none";
    case Node::Kind::kLiteral:
      return NumericLiteralCssText(node.value, node.unit);
    case Node::Kind::kOperation:
      break;
  }
  switch (node.op) {
    case CssMathOperator::kAdd:
    case CssMathOperator::kSubtract:
    case CssMathOperator::kMultiply: {
      std::vector<NodeWithOperator> terms =
          CollectSumOrProductInOrder(std::make_shared<Node>(node));
      if (terms.size() == 1) {
        terms = {{CssMathOperator::kMultiply, node.operands.front()},
                 {node.op, node.operands.back()}};
      }
      std::u16string result = u"(";
      result += NodeCssText(*MaybeNegateFirstNode(terms[0].op, terms[0].node));
      for (size_t i = 1; i < terms.size(); ++i) {
        CssMathOperator op = terms[i].op;
        NodePtr term = terms[i].node;
        if (op == CssMathOperator::kMultiply && IsInvert(*term)) {
          result += u" / ";
          result += NodeCssText(*term->operands.front());
          continue;
        }
        if (IsNumericLiteral(*term) &&
            (op == CssMathOperator::kAdd || op == CssMathOperator::kSubtract) &&
            term->value < 0.0) {
          op = op == CssMathOperator::kAdd ? CssMathOperator::kSubtract
                                           : CssMathOperator::kAdd;
          term = CreateLiteral(-term->value, term->unit);
        }
        result += u' ';
        result += OperatorName(op);
        result += u' ';
        result += NodeCssText(*term);
      }
      result += u')';
      return result;
    }
    case CssMathOperator::kMin:
    case CssMathOperator::kMax:
    case CssMathOperator::kClamp: {
      std::u16string result(OperatorName(node.op));
      result += u'(';
      SerializeTopLevelNode(*node.operands.front(), result);
      for (size_t i = 1; i < node.operands.size(); ++i) {
        result += u", ";
        SerializeTopLevelNode(*node.operands[i], result);
      }
      result += u')';
      return result;
    }
    case CssMathOperator::kInvert:
      return u"(1 / " + NodeCssText(*node.operands.front()) + u")";
    case CssMathOperator::kDivide:
      break;
  }
  return u"";
}

}  // namespace

// Upstream: third_party/blink/renderer/core/css/css_to_length_conversion_data.cc
double CssLengthResolver::ComputedPixels(double value, CssUnit unit) const {
  switch (unit) {
    case CssUnit::kEms:
    case CssUnit::kQuirkyEms:
      return value * em;
    case CssUnit::kExs:
      return value * ex;
    case CssUnit::kChs:
      return value * ch;
    case CssUnit::kIcs:
      return value * ic;
    case CssUnit::kCaps:
      return value * cap;
    case CssUnit::kLhs:
      return value * lh;
    case CssUnit::kRems:
      return value * rem;
    case CssUnit::kRexs:
      return value * rex;
    case CssUnit::kRchs:
      return value * rch;
    case CssUnit::kRics:
      return value * ric;
    case CssUnit::kRcaps:
      return value * rcap;
    case CssUnit::kRlhs:
      return value * rlh;
    case CssUnit::kViewportWidth:
    case CssUnit::kViewportInlineSize:
    case CssUnit::kSmallViewportWidth:
    case CssUnit::kSmallViewportInlineSize:
    case CssUnit::kLargeViewportWidth:
    case CssUnit::kLargeViewportInlineSize:
    case CssUnit::kDynamicViewportWidth:
    case CssUnit::kDynamicViewportInlineSize:
    case CssUnit::kContainerWidth:
    case CssUnit::kContainerInlineSize:
      return value * viewport_width / 100;
    case CssUnit::kViewportHeight:
    case CssUnit::kViewportBlockSize:
    case CssUnit::kSmallViewportHeight:
    case CssUnit::kSmallViewportBlockSize:
    case CssUnit::kLargeViewportHeight:
    case CssUnit::kLargeViewportBlockSize:
    case CssUnit::kDynamicViewportHeight:
    case CssUnit::kDynamicViewportBlockSize:
    case CssUnit::kContainerHeight:
    case CssUnit::kContainerBlockSize:
      return value * viewport_height / 100;
    case CssUnit::kViewportMin:
    case CssUnit::kSmallViewportMin:
    case CssUnit::kLargeViewportMin:
    case CssUnit::kDynamicViewportMin:
    case CssUnit::kContainerMin:
      return value * std::min(viewport_width, viewport_height) / 100;
    case CssUnit::kViewportMax:
    case CssUnit::kSmallViewportMax:
    case CssUnit::kLargeViewportMax:
    case CssUnit::kDynamicViewportMax:
    case CssUnit::kContainerMax:
      return value * std::max(viewport_width, viewport_height) / 100;
    default:
      return value * ConversionToCanonicalUnits(unit);
  }
}

// As Blink's UnitCategory().
CssCalcCategory CssCalcCategoryOf(CssUnit unit) {
  if (IsLengthUnit(unit)) {
    return unit == CssUnit::kQuirkyEms ? CssCalcCategory::kOther
                                       : CssCalcCategory::kLength;
  }
  switch (CssUnitCategoryOf(unit)) {
    case CssUnitCategory::kNumber:
      return CssCalcCategory::kNumber;
    case CssUnitCategory::kAngle:
      return CssCalcCategory::kAngle;
    case CssUnitCategory::kTime:
      return CssCalcCategory::kTime;
    case CssUnitCategory::kFrequency:
      return CssCalcCategory::kFrequency;
    case CssUnitCategory::kResolution:
      return CssCalcCategory::kResolution;
    default:
      return CssCalcCategory::kOther;
  }
}

CssMathNode::CssMathNode() = default;
CssMathNode::CssMathNode(const CssMathNode&) = default;
CssMathNode& CssMathNode::operator=(const CssMathNode&) = default;
CssMathNode::~CssMathNode() = default;

// As Blink's MathFunctionParser.
std::optional<CssMathFunction> CssMathFunction::Consume(CssTokenStream& stream,
                                                        bool integer,
                                                        bool non_negative) {
  const CssToken& token = stream.Peek();
  if (token.type != CssTokenType::kFunction) {
    return std::nullopt;
  }
  const CssTokenStream::State savepoint = stream.Save();
  const std::u16string name = token.value;
  NodePtr root;
  {
    CssTokenStream::BlockGuard guard(stream);
    stream.ConsumeWhitespace();
    root = Parser().ParseMathFunction(name, stream, Parser::State());
  }
  if (!root || root->kind == Node::Kind::kNone) {
    stream.Restore(savepoint);
    return std::nullopt;
  }
  stream.ConsumeWhitespace();
  return CssMathFunction(std::move(root), integer, non_negative);
}

CssMathFunction::CssMathFunction(std::shared_ptr<const CssMathNode> root,
                                 bool integer,
                                 bool non_negative)
    : root_(std::move(root)), integer_(integer), non_negative_(non_negative) {}

CssMathFunction::CssMathFunction(const CssMathFunction&) = default;
CssMathFunction& CssMathFunction::operator=(const CssMathFunction&) = default;
CssMathFunction::~CssMathFunction() = default;

// As Blink's CSSMathFunctionValue::CustomCSSText().
std::u16string CssMathFunction::CssText() const {
  const std::u16string text = NodeCssText(*root_);
  if (IsComparison(*root_)) {
    return text;
  }
  if (text.starts_with(u'(')) {
    return u"calc" + text;
  }
  return u"calc(" + text + u")";
}

std::optional<double> CssMathFunction::GetValueIfKnown() const {
  const std::optional<double> value = ValueInCanonicalUnit(*root_, nullptr);
  if (!value) {
    return std::nullopt;
  }
  // As CSSValueClampingUtils::ClampDouble() holds it to finite values.
  return ClampToPermittedRange(std::clamp(*value,
                                          std::numeric_limits<double>::lowest(),
                                          std::numeric_limits<double>::max()));
}

double CssMathFunction::ComputeValueInCanonicalUnit(
    const CssLengthResolver& resolver) const {
  return ClampToPermittedRange(
      ValueInCanonicalUnit(*root_, &resolver).value_or(0));
}

// As Blink's CSSMathFunctionValue::ClampToPermittedRange().
double CssMathFunction::ClampToPermittedRange(double value) const {
  if (std::isnan(value)) {
    value = 0;
  }
  if (non_negative_) {
    value = std::max(value, 0.0);
  }
  if (integer_) {
    value = std::floor(value + 0.5);
  }
  return value;
}

}  // namespace mitto
