#include "mitto/css/media_query.h"

#include <utility>

#include "base/check.h"
#include "mitto/css/css_serialization.h"

namespace mitto {

namespace {

// As Blink's KleeneAnd(), KleeneOr() and KleeneNot().
KleeneValue KleeneAnd(KleeneValue a, KleeneValue b) {
  if (a == KleeneValue::kFalse || b == KleeneValue::kFalse) {
    return KleeneValue::kFalse;
  }
  if (a == KleeneValue::kUnknown || b == KleeneValue::kUnknown) {
    return KleeneValue::kUnknown;
  }
  return KleeneValue::kTrue;
}

KleeneValue KleeneOr(KleeneValue a, KleeneValue b) {
  if (a == KleeneValue::kTrue || b == KleeneValue::kTrue) {
    return KleeneValue::kTrue;
  }
  if (a == KleeneValue::kUnknown || b == KleeneValue::kUnknown) {
    return KleeneValue::kUnknown;
  }
  return KleeneValue::kFalse;
}

KleeneValue KleeneNot(KleeneValue a) {
  switch (a) {
    case KleeneValue::kTrue:
      return KleeneValue::kFalse;
    case KleeneValue::kFalse:
      return KleeneValue::kTrue;
    case KleeneValue::kUnknown:
      return KleeneValue::kUnknown;
  }
  return KleeneValue::kUnknown;
}

std::u16string_view OperatorText(MediaQueryOperator op) {
  switch (op) {
    case MediaQueryOperator::kNone:
      return u"";
    case MediaQueryOperator::kEq:
      return u"=";
    case MediaQueryOperator::kLt:
      return u"<";
    case MediaQueryOperator::kLe:
      return u"<=";
    case MediaQueryOperator::kGt:
      return u">";
    case MediaQueryOperator::kGe:
      return u">=";
  }
  return u"";
}

class FeatureNode final : public ConditionalExpNode {
 public:
  explicit FeatureNode(MediaQueryExp exp) : exp_(std::move(exp)) {}

  KleeneValue Evaluate(ConditionalExpNodeVisitor& visitor) const override {
    return visitor.EvaluateFeature(exp_);
  }
  void SerializeTo(std::u16string& builder) const override {
    builder += exp_.Serialize();
  }
  bool ContainsUnknown() const override { return false; }

 private:
  const MediaQueryExp exp_;
};

// <general-enclosed>, which is written back as it was given.
class UnknownNode final : public ConditionalExpNode {
 public:
  explicit UnknownNode(std::u16string text) : text_(std::move(text)) {}

  KleeneValue Evaluate(ConditionalExpNodeVisitor& visitor) const override {
    return KleeneValue::kUnknown;
  }
  void SerializeTo(std::u16string& builder) const override { builder += text_; }
  bool ContainsUnknown() const override { return true; }

 private:
  const std::u16string text_;
};

class NotNode final : public ConditionalExpNode {
 public:
  explicit NotNode(std::unique_ptr<ConditionalExpNode> operand)
      : operand_(std::move(operand)) {}

  KleeneValue Evaluate(ConditionalExpNodeVisitor& visitor) const override {
    return KleeneNot(operand_->Evaluate(visitor));
  }
  void SerializeTo(std::u16string& builder) const override {
    builder += u"not ";
    operand_->SerializeTo(builder);
  }
  bool ContainsUnknown() const override { return operand_->ContainsUnknown(); }

 private:
  const std::unique_ptr<ConditionalExpNode> operand_;
};

class NestedNode final : public ConditionalExpNode {
 public:
  explicit NestedNode(std::unique_ptr<ConditionalExpNode> operand)
      : operand_(std::move(operand)) {}

  KleeneValue Evaluate(ConditionalExpNodeVisitor& visitor) const override {
    return operand_->Evaluate(visitor);
  }
  void SerializeTo(std::u16string& builder) const override {
    builder += u"(";
    operand_->SerializeTo(builder);
    builder += u")";
  }
  bool ContainsUnknown() const override { return operand_->ContainsUnknown(); }

 private:
  const std::unique_ptr<ConditionalExpNode> operand_;
};

class AndNode final : public ConditionalExpNode {
 public:
  AndNode(std::unique_ptr<ConditionalExpNode> left,
          std::unique_ptr<ConditionalExpNode> right)
      : left_(std::move(left)), right_(std::move(right)) {}

  KleeneValue Evaluate(ConditionalExpNodeVisitor& visitor) const override {
    const KleeneValue left = left_->Evaluate(visitor);
    if (left == KleeneValue::kFalse) {
      return left;
    }
    return KleeneAnd(left, right_->Evaluate(visitor));
  }
  void SerializeTo(std::u16string& builder) const override {
    left_->SerializeTo(builder);
    builder += u" and ";
    right_->SerializeTo(builder);
  }
  bool ContainsUnknown() const override {
    return left_->ContainsUnknown() || right_->ContainsUnknown();
  }

 private:
  const std::unique_ptr<ConditionalExpNode> left_;
  const std::unique_ptr<ConditionalExpNode> right_;
};

class OrNode final : public ConditionalExpNode {
 public:
  OrNode(std::unique_ptr<ConditionalExpNode> left,
         std::unique_ptr<ConditionalExpNode> right)
      : left_(std::move(left)), right_(std::move(right)) {}

  KleeneValue Evaluate(ConditionalExpNodeVisitor& visitor) const override {
    const KleeneValue left = left_->Evaluate(visitor);
    if (left == KleeneValue::kTrue) {
      return left;
    }
    return KleeneOr(left, right_->Evaluate(visitor));
  }
  void SerializeTo(std::u16string& builder) const override {
    left_->SerializeTo(builder);
    builder += u" or ";
    right_->SerializeTo(builder);
  }
  bool ContainsUnknown() const override {
    return left_->ContainsUnknown() || right_->ContainsUnknown();
  }

 private:
  const std::unique_ptr<ConditionalExpNode> left_;
  const std::unique_ptr<ConditionalExpNode> right_;
};

}  // namespace

CssPrimitiveValue CssPrimitiveValue::Literal(double value, CssUnit unit) {
  return CssPrimitiveValue(value, unit);
}

CssPrimitiveValue CssPrimitiveValue::Math(CssMathFunction math) {
  return CssPrimitiveValue(std::move(math));
}

CssPrimitiveValue::CssPrimitiveValue(double value, CssUnit unit)
    : value_(value), unit_(unit) {}

CssPrimitiveValue::CssPrimitiveValue(CssMathFunction math)
    : math_(std::move(math)) {}

CssPrimitiveValue::CssPrimitiveValue(const CssPrimitiveValue&) = default;
CssPrimitiveValue& CssPrimitiveValue::operator=(const CssPrimitiveValue&) =
    default;
CssPrimitiveValue::~CssPrimitiveValue() = default;

bool CssPrimitiveValue::IsNumber() const {
  if (math_) {
    return math_->Category() == CssCalcCategory::kNumber;
  }
  return unit_ == CssUnit::kNumber || unit_ == CssUnit::kInteger;
}

bool CssPrimitiveValue::IsInteger() const {
  if (math_) {
    return math_->Category() == CssCalcCategory::kNumber;
  }
  return unit_ == CssUnit::kInteger;
}

bool CssPrimitiveValue::IsLength() const {
  if (math_) {
    return math_->Category() == CssCalcCategory::kLength;
  }
  return IsLengthUnit(unit_);
}

bool CssPrimitiveValue::IsResolution() const {
  if (math_) {
    return math_->Category() == CssCalcCategory::kResolution;
  }
  return IsResolutionUnit(unit_);
}

bool CssPrimitiveValue::IsDotsPerCentimeter() const {
  return !math_ && unit_ == CssUnit::kDotsPerCentimeter;
}

std::optional<double> CssPrimitiveValue::GetValueIfKnown() const {
  if (math_) {
    return math_->GetValueIfKnown();
  }
  return value_;
}

// As Blink's CSSNumericLiteralValue::ComputeValueInCanonicalUnit().
double CssPrimitiveValue::ComputeValueInCanonicalUnit(
    const CssLengthResolver& resolver) const {
  if (math_) {
    return math_->ComputeValueInCanonicalUnit(resolver);
  }
  if (IsLengthUnit(unit_)) {
    return resolver.ComputedPixels(value_, unit_);
  }
  return value_ * ConversionToCanonicalUnits(unit_);
}

std::u16string CssPrimitiveValue::CssText() const {
  if (math_) {
    return math_->CssText();
  }
  return NumericLiteralCssText(value_, unit_);
}

MediaQueryExpValue::MediaQueryExpValue() = default;

MediaQueryExpValue::MediaQueryExpValue(CssPrimitiveValue value)
    : type_(Type::kValue), value_(std::move(value)) {}

MediaQueryExpValue::MediaQueryExpValue(CssPrimitiveValue numerator,
                                       CssPrimitiveValue denominator)
    : type_(Type::kRatio),
      value_(std::move(numerator)),
      denominator_(std::move(denominator)) {}

MediaQueryExpValue::MediaQueryExpValue(std::u16string id)
    : type_(Type::kId), id_(std::move(id)) {}

MediaQueryExpValue::MediaQueryExpValue(const MediaQueryExpValue&) = default;
MediaQueryExpValue& MediaQueryExpValue::operator=(const MediaQueryExpValue&) =
    default;
MediaQueryExpValue::~MediaQueryExpValue() = default;

double MediaQueryExpValue::Value(const CssLengthResolver& resolver) const {
  DCHECK(IsValue());
  return value_->ComputeValueInCanonicalUnit(resolver);
}

double MediaQueryExpValue::Numerator(const CssLengthResolver& resolver) const {
  DCHECK(IsRatio());
  return value_->ComputeValueInCanonicalUnit(resolver);
}

double MediaQueryExpValue::Denominator(
    const CssLengthResolver& resolver) const {
  DCHECK(IsRatio());
  return denominator_->ComputeValueInCanonicalUnit(resolver);
}

std::u16string MediaQueryExpValue::CssText() const {
  switch (type_) {
    case Type::kInvalid:
      return u"";
    case Type::kValue:
      return value_->CssText();
    case Type::kRatio:
      return value_->CssText() + u" / " + denominator_->CssText();
    case Type::kId:
      return id_;
  }
  return u"";
}

MediaQueryExpComparison::MediaQueryExpComparison() = default;
MediaQueryExpComparison::MediaQueryExpComparison(MediaQueryExpValue value,
                                                 MediaQueryOperator op)
    : value(std::move(value)), op(op) {}
MediaQueryExpComparison::MediaQueryExpComparison(
    const MediaQueryExpComparison&) = default;
MediaQueryExpComparison& MediaQueryExpComparison::operator=(
    const MediaQueryExpComparison&) = default;
MediaQueryExpComparison::~MediaQueryExpComparison() = default;

MediaQueryExpBounds::MediaQueryExpBounds() = default;
MediaQueryExpBounds::MediaQueryExpBounds(MediaQueryExpComparison right)
    : right(std::move(right)) {}
MediaQueryExpBounds::MediaQueryExpBounds(MediaQueryExpComparison left,
                                         MediaQueryExpComparison right)
    : left(std::move(left)), right(std::move(right)) {}
MediaQueryExpBounds::MediaQueryExpBounds(const MediaQueryExpBounds&) = default;
MediaQueryExpBounds& MediaQueryExpBounds::operator=(
    const MediaQueryExpBounds&) = default;
MediaQueryExpBounds::~MediaQueryExpBounds() = default;

MediaQueryExp::MediaQueryExp() = default;
MediaQueryExp::MediaQueryExp(std::u16string feature, MediaQueryExpBounds bounds)
    : feature(std::move(feature)), bounds(std::move(bounds)) {}
MediaQueryExp::MediaQueryExp(const MediaQueryExp&) = default;
MediaQueryExp& MediaQueryExp::operator=(const MediaQueryExp&) = default;
MediaQueryExp::~MediaQueryExp() = default;

// As Blink's MediaQueryExp::Serialize().
std::u16string MediaQueryExp::Serialize() const {
  std::u16string result;
  if (!bounds.IsRange()) {
    // <mf-boolean>, such as (color), or <mf-plain>, such as (width: 100px).
    SerializeIdentifier(feature, result);
    if (bounds.right.IsValid()) {
      result += u": ";
      result += bounds.right.value.CssText();
    }
    return result;
  }
  if (bounds.left.IsValid()) {
    result += bounds.left.value.CssText();
    result += u" ";
    result += OperatorText(bounds.left.op);
    result += u" ";
  }
  SerializeIdentifier(feature, result);
  if (bounds.right.IsValid()) {
    result += u" ";
    result += OperatorText(bounds.right.op);
    result += u" ";
    result += bounds.right.value.CssText();
  }
  return result;
}

std::u16string ConditionalExpNode::Serialize() const {
  std::u16string builder;
  SerializeTo(builder);
  return builder;
}

std::unique_ptr<ConditionalExpNode> ConditionalExpNode::Feature(
    MediaQueryExp exp) {
  return std::make_unique<FeatureNode>(std::move(exp));
}

std::unique_ptr<ConditionalExpNode> ConditionalExpNode::Unknown(
    std::u16string text) {
  return std::make_unique<UnknownNode>(std::move(text));
}

std::unique_ptr<ConditionalExpNode> ConditionalExpNode::Not(
    std::unique_ptr<ConditionalExpNode> operand) {
  if (!operand) {
    return nullptr;
  }
  return std::make_unique<NotNode>(std::move(operand));
}

std::unique_ptr<ConditionalExpNode> ConditionalExpNode::Nested(
    std::unique_ptr<ConditionalExpNode> operand) {
  if (!operand) {
    return nullptr;
  }
  return std::make_unique<NestedNode>(std::move(operand));
}

std::unique_ptr<ConditionalExpNode> ConditionalExpNode::And(
    std::unique_ptr<ConditionalExpNode> left,
    std::unique_ptr<ConditionalExpNode> right) {
  if (!left || !right) {
    return nullptr;
  }
  return std::make_unique<AndNode>(std::move(left), std::move(right));
}

std::unique_ptr<ConditionalExpNode> ConditionalExpNode::Or(
    std::unique_ptr<ConditionalExpNode> left,
    std::unique_ptr<ConditionalExpNode> right) {
  if (!left || !right) {
    return nullptr;
  }
  return std::make_unique<OrNode>(std::move(left), std::move(right));
}

std::unique_ptr<MediaQuery> MediaQuery::CreateNotAll() {
  return std::make_unique<MediaQuery>(RestrictorType::kNot, u"all", nullptr);
}

MediaQuery::MediaQuery(RestrictorType restrictor,
                       std::u16string media_type,
                       std::unique_ptr<ConditionalExpNode> exp_node)
    : restrictor_(restrictor),
      media_type_(std::move(media_type)),
      exp_node_(std::move(exp_node)) {
  for (char16_t& c : media_type_) {
    if (c >= 'A' && c <= 'Z') {
      c += 'a' - 'A';
    }
  }
}

MediaQuery::~MediaQuery() = default;

// As Blink's MediaQuery::Serialize().
std::u16string MediaQuery::CssText() const {
  std::u16string result;
  switch (restrictor_) {
    case RestrictorType::kOnly:
      result += u"only ";
      break;
    case RestrictorType::kNot:
      result += u"not ";
      break;
    case RestrictorType::kNone:
      break;
  }
  if (!exp_node_) {
    SerializeIdentifier(media_type_, result);
    return result;
  }
  if (media_type_ != u"all" || restrictor_ != RestrictorType::kNone) {
    SerializeIdentifier(media_type_, result);
    result += u" and ";
  }
  result += exp_node_->Serialize();
  return result;
}

MediaQuerySet::MediaQuerySet() = default;
MediaQuerySet::MediaQuerySet(std::vector<std::unique_ptr<MediaQuery>> queries)
    : queries_(std::move(queries)) {}
MediaQuerySet::~MediaQuerySet() = default;

std::u16string MediaQuerySet::MediaText() const {
  std::u16string text;
  for (size_t i = 0; i < queries_.size(); ++i) {
    if (i > 0) {
      text += u", ";
    }
    text += queries_[i]->CssText();
  }
  return text;
}

}  // namespace mitto
