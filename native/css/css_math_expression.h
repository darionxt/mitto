// CSS's math functions as Blink parses, simplifies, writes and computes them,
// for what a media query takes: calc(), -webkit-calc(), min(), max() and
// clamp(), of numbers and dimensions, with +, -, * and /, parentheses and the
// constants e, pi, infinity, -infinity and NaN.
//
// The other math functions, such as round(), sin() or pow(), percentages,
// and a product or quotient of two dimensions, which Blink's typed arithmetic
// takes, are Known Gaps: they do not parse, so a media feature that holds one
// is unknown, where Chrome would compute it. Blink's own
// CSSMathExpressionNode handles every property's values; M9's CSS parser
// takes this further.
//
// Upstream: third_party/blink/renderer/core/css/css_math_expression_node.cc
// Upstream: third_party/blink/renderer/core/css/css_math_function_value.cc
// Upstream: third_party/blink/renderer/core/css/css_math_operator.cc

#ifndef MITTO_CSS_CSS_MATH_EXPRESSION_H_
#define MITTO_CSS_CSS_MATH_EXPRESSION_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mitto/css/css_token.h"

namespace mitto {

class CssTokenStream;

// What relative lengths resolve against, as Blink's CSSLengthResolver gives
// them, in pixels.
struct CssLengthResolver {
  double em = 16;
  double rem = 16;
  double ex = 8;
  double rex = 8;
  double ch = 8;
  double rch = 8;
  double ic = 16;
  double ric = 16;
  double cap = 0;
  double rcap = 0;
  double lh = 0;
  double rlh = 0;
  // The viewport, which the small, large and dynamic viewports and a media
  // query's container all equal in a desktop window.
  double viewport_width = 0;
  double viewport_height = 0;

  // A length in `unit` in pixels, as Blink's ZoomedComputedPixels() gives it
  // at a zoom of 1.
  double ComputedPixels(double value, CssUnit unit) const;
};

// What a math expression's result is, as Blink's CalculationResultCategory.
enum class CssCalcCategory {
  kNumber,
  kLength,
  kAngle,
  kTime,
  kFrequency,
  kResolution,
  kOther,
};

CssCalcCategory CssCalcCategoryOf(CssUnit unit);

enum class CssMathOperator {
  kAdd,
  kSubtract,
  kMultiply,
  kDivide,
  kInvert,
  kMin,
  kMax,
  kClamp,
};

// A node of a math expression: a numeric literal, the keyword none that
// clamp() takes, or an operation on other nodes.
struct CssMathNode {
  enum class Kind {
    kLiteral,
    kNone,
    kOperation,
  };

  CssMathNode();
  CssMathNode(const CssMathNode&);
  CssMathNode& operator=(const CssMathNode&);
  ~CssMathNode();

  Kind kind = Kind::kLiteral;
  CssCalcCategory category = CssCalcCategory::kNumber;
  // A literal's value and unit.
  double value = 0;
  CssUnit unit = CssUnit::kNumber;
  // An operation's operator and operands, two for an arithmetic operator, one
  // for kInvert and one or more for a comparison.
  CssMathOperator op = CssMathOperator::kAdd;
  std::vector<std::shared_ptr<CssMathNode>> operands;
  // Whether the node stands in parentheses of its own, which changes the
  // signs of what a subtraction in front of it subtracts.
  bool is_nested_calc = false;
};

// A math function's value, as Blink's CSSMathFunctionValue holds one.
class CssMathFunction {
 public:
  // Parses the math function that starts at the next token, which must be a
  // function token, and consumes it, or returns nullopt and consumes nothing.
  // `integer` rounds the result as Blink does for a function that stands
  // where an integer goes, and `non_negative` holds it to 0 and up.
  static std::optional<CssMathFunction> Consume(CssTokenStream& stream,
                                                bool integer,
                                                bool non_negative);

  CssMathFunction(const CssMathFunction&);
  CssMathFunction& operator=(const CssMathFunction&);
  ~CssMathFunction();

  CssCalcCategory Category() const { return root_->category; }
  // As Blink writes it: "calc(3dppx)" or "min(1px, 2em)".
  std::u16string CssText() const;
  // The value in its category's canonical unit, if it needs nothing to
  // resolve relative lengths against.
  std::optional<double> GetValueIfKnown() const;
  double ComputeValueInCanonicalUnit(const CssLengthResolver& resolver) const;

 private:
  CssMathFunction(std::shared_ptr<const CssMathNode> root,
                  bool integer,
                  bool non_negative);

  double ClampToPermittedRange(double value) const;

  std::shared_ptr<const CssMathNode> root_;
  bool integer_;
  bool non_negative_;
};

}  // namespace mitto

#endif  // MITTO_CSS_CSS_MATH_EXPRESSION_H_
