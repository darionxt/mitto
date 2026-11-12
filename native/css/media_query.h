// A list of media queries, their conditions and their media features, as
// Blink's MediaQuerySet, MediaQuery, ConditionalExpNode and MediaQueryExp
// hold and write them:
// https://drafts.csswg.org/mediaqueries-5/
//
// Upstream: third_party/blink/renderer/core/css/media_list.cc
// Upstream: third_party/blink/renderer/core/css/media_query.cc
// Upstream: third_party/blink/renderer/core/css/media_query_exp.cc
// Upstream: third_party/blink/renderer/core/css/conditional_exp_node.cc

#ifndef MITTO_CSS_MEDIA_QUERY_H_
#define MITTO_CSS_MEDIA_QUERY_H_

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "mitto/css/css_math_expression.h"
#include "mitto/css/css_token.h"

namespace mitto {

// Three-valued logic, which <general-enclosed> needs: a condition that Chrome
// does not know is unknown, and so is "not" of it.
enum class KleeneValue {
  kTrue,
  kFalse,
  kUnknown,
};

// A number, length or resolution, written or a math function's, as Blink's
// CSSPrimitiveValue.
class CssPrimitiveValue {
 public:
  static CssPrimitiveValue Literal(double value, CssUnit unit);
  static CssPrimitiveValue Math(CssMathFunction math);

  CssPrimitiveValue(const CssPrimitiveValue&);
  CssPrimitiveValue& operator=(const CssPrimitiveValue&);
  ~CssPrimitiveValue();

  bool IsNumber() const;
  // Where an integer goes, a math function of a number counts as one.
  bool IsInteger() const;
  bool IsLength() const;
  bool IsResolution() const;
  bool IsDotsPerCentimeter() const;
  std::optional<double> GetValueIfKnown() const;
  double ComputeValueInCanonicalUnit(const CssLengthResolver& resolver) const;
  std::u16string CssText() const;

 private:
  CssPrimitiveValue(double value, CssUnit unit);
  explicit CssPrimitiveValue(CssMathFunction math);

  double value_ = 0;
  CssUnit unit_ = CssUnit::kNumber;
  std::optional<CssMathFunction> math_;
};

// What a media feature compares with: a value, a ratio or a keyword.
class MediaQueryExpValue {
 public:
  enum class Type {
    kInvalid,
    kValue,
    kRatio,
    kId,
  };

  MediaQueryExpValue();
  explicit MediaQueryExpValue(CssPrimitiveValue value);
  MediaQueryExpValue(CssPrimitiveValue numerator,
                     CssPrimitiveValue denominator);
  // A keyword, in lower case.
  explicit MediaQueryExpValue(std::u16string id);
  MediaQueryExpValue(const MediaQueryExpValue&);
  MediaQueryExpValue& operator=(const MediaQueryExpValue&);
  ~MediaQueryExpValue();

  bool IsValid() const { return type_ != Type::kInvalid; }
  bool IsId() const { return type_ == Type::kId; }
  bool IsValue() const { return type_ == Type::kValue; }
  bool IsRatio() const { return type_ == Type::kRatio; }
  bool IsNumber() const { return IsValue() && value_->IsNumber(); }
  bool IsResolution() const { return IsValue() && value_->IsResolution(); }
  bool IsDotsPerCentimeter() const {
    return IsValue() && value_->IsDotsPerCentimeter();
  }

  const std::u16string& Id() const { return id_; }
  double Value(const CssLengthResolver& resolver) const;
  double Numerator(const CssLengthResolver& resolver) const;
  double Denominator(const CssLengthResolver& resolver) const;
  std::u16string CssText() const;

 private:
  Type type_ = Type::kInvalid;
  std::optional<CssPrimitiveValue> value_;
  std::optional<CssPrimitiveValue> denominator_;
  std::u16string id_;
};

enum class MediaQueryOperator {
  kNone,
  kEq,
  kLt,
  kLe,
  kGt,
  kGe,
};

struct MediaQueryExpComparison {
  MediaQueryExpComparison();
  explicit MediaQueryExpComparison(
      MediaQueryExpValue value,
      MediaQueryOperator op = MediaQueryOperator::kNone);
  MediaQueryExpComparison(const MediaQueryExpComparison&);
  MediaQueryExpComparison& operator=(const MediaQueryExpComparison&);
  ~MediaQueryExpComparison();

  bool IsValid() const { return value.IsValid(); }

  MediaQueryExpValue value;
  MediaQueryOperator op = MediaQueryOperator::kNone;
};

// The comparisons of a media feature: `right` alone for (width: 10px) and
// (width < 10px), `left` alone for (10px < width), and both for
// (10px < width < 20px).
struct MediaQueryExpBounds {
  MediaQueryExpBounds();
  explicit MediaQueryExpBounds(MediaQueryExpComparison right);
  MediaQueryExpBounds(MediaQueryExpComparison left,
                      MediaQueryExpComparison right);
  MediaQueryExpBounds(const MediaQueryExpBounds&);
  MediaQueryExpBounds& operator=(const MediaQueryExpBounds&);
  ~MediaQueryExpBounds();

  bool IsRange() const {
    return left.op != MediaQueryOperator::kNone ||
           right.op != MediaQueryOperator::kNone;
  }

  MediaQueryExpComparison left;
  MediaQueryExpComparison right;
};

// A media feature: its name, in lower case, and its bounds.
struct MediaQueryExp {
  MediaQueryExp();
  MediaQueryExp(std::u16string feature, MediaQueryExpBounds bounds);
  MediaQueryExp(const MediaQueryExp&);
  MediaQueryExp& operator=(const MediaQueryExp&);
  ~MediaQueryExp();

  std::u16string Serialize() const;

  std::u16string feature;
  MediaQueryExpBounds bounds;
};

class ConditionalExpNode;

// What a condition's media features evaluate to.
class ConditionalExpNodeVisitor {
 public:
  virtual ~ConditionalExpNodeVisitor() = default;
  virtual KleeneValue EvaluateFeature(const MediaQueryExp& exp) = 0;
};

class ConditionalExpNode {
 public:
  virtual ~ConditionalExpNode() = default;

  virtual KleeneValue Evaluate(ConditionalExpNodeVisitor& visitor) const = 0;
  virtual void SerializeTo(std::u16string& builder) const = 0;
  // Whether the condition has a <general-enclosed> anywhere in it.
  virtual bool ContainsUnknown() const = 0;
  std::u16string Serialize() const;

  // Each returns nullptr if an operand is nullptr, as Blink's do.
  static std::unique_ptr<ConditionalExpNode> Feature(MediaQueryExp exp);
  static std::unique_ptr<ConditionalExpNode> Unknown(std::u16string text);
  static std::unique_ptr<ConditionalExpNode> Not(
      std::unique_ptr<ConditionalExpNode> operand);
  static std::unique_ptr<ConditionalExpNode> Nested(
      std::unique_ptr<ConditionalExpNode> operand);
  static std::unique_ptr<ConditionalExpNode> And(
      std::unique_ptr<ConditionalExpNode> left,
      std::unique_ptr<ConditionalExpNode> right);
  static std::unique_ptr<ConditionalExpNode> Or(
      std::unique_ptr<ConditionalExpNode> left,
      std::unique_ptr<ConditionalExpNode> right);
};

class MediaQuery {
 public:
  enum class RestrictorType {
    kOnly,
    kNot,
    kNone,
  };

  // not all, which a query that does not parse becomes.
  static std::unique_ptr<MediaQuery> CreateNotAll();

  MediaQuery(RestrictorType restrictor,
             std::u16string media_type,
             std::unique_ptr<ConditionalExpNode> exp_node);
  MediaQuery(const MediaQuery&) = delete;
  MediaQuery& operator=(const MediaQuery&) = delete;
  ~MediaQuery();

  RestrictorType Restrictor() const { return restrictor_; }
  // In lower case.
  const std::u16string& MediaType() const { return media_type_; }
  const ConditionalExpNode* ExpNode() const { return exp_node_.get(); }
  // https://drafts.csswg.org/cssom/#serialize-a-media-query
  std::u16string CssText() const;

 private:
  RestrictorType restrictor_;
  std::u16string media_type_;
  std::unique_ptr<ConditionalExpNode> exp_node_;
};

class MediaQuerySet {
 public:
  MediaQuerySet();
  explicit MediaQuerySet(std::vector<std::unique_ptr<MediaQuery>> queries);
  MediaQuerySet(const MediaQuerySet&) = delete;
  MediaQuerySet& operator=(const MediaQuerySet&) = delete;
  ~MediaQuerySet();

  const std::vector<std::unique_ptr<MediaQuery>>& Queries() const {
    return queries_;
  }
  // https://drafts.csswg.org/cssom/#serialize-a-media-query-list
  std::u16string MediaText() const;

 private:
  std::vector<std::unique_ptr<MediaQuery>> queries_;
};

}  // namespace mitto

#endif  // MITTO_CSS_MEDIA_QUERY_H_
