#include "mitto/dom/svg_transforms.h"

#include <cmath>
#include <limits>
#include <utility>

#include "base/notreached.h"
#include "base/numerics/angle_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_svg_animated_transform_list.h"
#include "mitto/bindings/v8_svg_matrix.h"
#include "mitto/bindings/v8_svg_transform.h"
#include "mitto/bindings/v8_svg_transform_list.h"
#include "mitto/dom/svg_element.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

namespace {

void ThrowReadOnly(bindings::ExceptionState& exception_state) {
  exception_state.ThrowDOMException("NoModificationAllowedError",
                                    "This object is read-only.");
}

// ClampTo<float>(), which keeps NaN.
float ClampToFloat(double value) {
  constexpr double kMax = std::numeric_limits<float>::max();
  if (value >= kMax) {
    return std::numeric_limits<float>::max();
  }
  if (value <= -kMax) {
    return -std::numeric_limits<float>::max();
  }
  return static_cast<float>(value);
}

// ClampToWithNaNTo0<float>().
float ClampToFloatWithNaNTo0(double value) {
  return std::isnan(value) ? 0 : ClampToFloat(value);
}

// DoMultiply(): t1 * t2.
SVGAffineTransform Multiply(const SVGAffineTransform& t1,
                            const SVGAffineTransform& t2) {
  if (t1.IsIdentityOrTranslation()) {
    return SVGAffineTransform(t2.A(), t2.B(), t2.C(), t2.D(), t1.E() + t2.E(),
                              t1.F() + t2.F());
  }
  return SVGAffineTransform(
      t1.A() * t2.A() + t1.C() * t2.B(), t1.B() * t2.A() + t1.D() * t2.B(),
      t1.A() * t2.C() + t1.C() * t2.D(), t1.B() * t2.C() + t1.D() * t2.D(),
      t1.A() * t2.E() + t1.C() * t2.F() + t1.E(),
      t1.B() * t2.E() + t1.D() * t2.F() + t1.F());
}

// AffineTransform::MakeRotationAroundPoint().
SVGAffineTransform RotationAroundPoint(double angle, double cx, double cy) {
  SVGAffineTransform result(1, 0, 0, 1, cx, cy);
  result.Rotate(angle);
  result.Translate(-cx, -cy);
  return result;
}

// DecomposeRotationCenter(): the center of a rotation of `angle` that
// `matrix` is, found again from its translation. Blink turns the float angle
// into radians in floats.
std::array<float, 2> RotationCenter(const SVGAffineTransform& matrix,
                                    float angle) {
  const double angle_in_rad = base::DegToRad(angle);
  const double cos_angle = std::cos(angle_in_rad);
  const double sin_angle = std::sin(angle_in_rad);
  if (cos_angle == 1) {
    return {0, 0};
  }
  const double e = matrix.E();
  const double f = matrix.F();
  const double cx = (e * (1 - cos_angle) - f * sin_angle) / (1 - cos_angle) / 2;
  const double cy = (e * sin_angle / (1 - cos_angle) + f) / 2;
  return {ClampToFloat(cx), ClampToFloat(cy)};
}

}  // namespace

// SVGAffineTransform.

bool SVGAffineTransform::IsIdentityOrTranslation() const {
  return values_[0] == 1 && values_[1] == 0 && values_[2] == 0 &&
         values_[3] == 1;
}

bool SVGAffineTransform::IsInvertible() const {
  return std::isnormal(values_[0] * values_[3] - values_[1] * values_[2]);
}

SVGAffineTransform SVGAffineTransform::Inverse() const {
  SVGAffineTransform result;
  if (IsIdentityOrTranslation()) {
    result.values_[4] = -values_[4];
    result.values_[5] = -values_[5];
    return result;
  }
  const double determinant = values_[0] * values_[3] - values_[1] * values_[2];
  if (!std::isnormal(determinant)) {
    return result;
  }
  result.values_[0] = values_[3] / determinant;
  result.values_[1] = -values_[1] / determinant;
  result.values_[2] = -values_[2] / determinant;
  result.values_[3] = values_[0] / determinant;
  result.values_[4] =
      (values_[2] * values_[5] - values_[3] * values_[4]) / determinant;
  result.values_[5] =
      (values_[1] * values_[4] - values_[0] * values_[5]) / determinant;
  return result;
}

SVGAffineTransform& SVGAffineTransform::PreConcat(
    const SVGAffineTransform& other) {
  *this = Multiply(*this, other);
  return *this;
}

SVGAffineTransform& SVGAffineTransform::Rotate(double angle) {
  return RotateRadians(base::DegToRad(angle));
}

SVGAffineTransform& SVGAffineTransform::RotateRadians(double angle) {
  const double cos_angle = std::cos(angle);
  const double sin_angle = std::sin(angle);
  return PreConcat(
      SVGAffineTransform(cos_angle, sin_angle, -sin_angle, cos_angle, 0, 0));
}

SVGAffineTransform& SVGAffineTransform::Scale(double sx, double sy) {
  values_[0] *= sx;
  values_[1] *= sx;
  values_[2] *= sy;
  values_[3] *= sy;
  return *this;
}

SVGAffineTransform& SVGAffineTransform::Translate(double tx, double ty) {
  values_[4] += tx * values_[0] + ty * values_[2];
  values_[5] += tx * values_[1] + ty * values_[3];
  return *this;
}

SVGAffineTransform& SVGAffineTransform::RotateFromVector(double x, double y) {
  return RotateRadians(std::atan2(y, x));
}

SVGAffineTransform& SVGAffineTransform::Shear(double sx, double sy) {
  const double a = values_[0];
  const double b = values_[1];
  values_[0] += sy * values_[2];
  values_[1] += sy * values_[3];
  values_[2] += sx * a;
  values_[3] += sx * b;
  return *this;
}

SVGAffineTransform& SVGAffineTransform::SkewX(double angle) {
  return Shear(std::tan(base::DegToRad(angle)), 0);
}

SVGAffineTransform& SVGAffineTransform::SkewY(double angle) {
  return Shear(0, std::tan(base::DegToRad(angle)));
}

std::array<float, 2> SVGAffineTransform::MapPoint(
    const std::array<float, 2>& point) const {
  return {ClampToFloatWithNaNTo0(values_[0] * point[0] + values_[2] * point[1] +
                                 values_[4]),
          ClampToFloatWithNaNTo0(values_[1] * point[0] + values_[3] * point[1] +
                                 values_[5])};
}

// SVGTransformValue.

// static
SVGTransformValue SVGTransformValue::FromArguments(
    const SVGTransformArguments& parsed) {
  SVGTransformValue value;
  value.type = static_cast<Type>(parsed.type);
  const std::array<float, 6>& v = parsed.values;
  switch (value.type) {
    case kSkewX:
      value.angle = v[0];
      value.matrix = SVGAffineTransform(
          1, 0, std::tan(base::DegToRad<double>(v[0])), 1, 0, 0);
      break;
    case kSkewY:
      value.angle = v[0];
      value.matrix = SVGAffineTransform(
          1, std::tan(base::DegToRad<double>(v[0])), 0, 1, 0, 0);
      break;
    case kScale:
      // One argument scales uniformly.
      value.matrix =
          SVGAffineTransform(v[0], 0, 0, parsed.count == 1 ? v[0] : v[1], 0, 0);
      break;
    case kTranslate:
      // One argument translates along x alone.
      value.matrix =
          SVGAffineTransform(1, 0, 0, 1, v[0], parsed.count == 1 ? 0 : v[1]);
      break;
    case kRotate:
      value.angle = v[0];
      value.matrix = parsed.count == 1 ? RotationAroundPoint(v[0], 0, 0)
                                       : RotationAroundPoint(v[0], v[1], v[2]);
      break;
    case kMatrix:
      value.matrix = SVGAffineTransform(v[0], v[1], v[2], v[3], v[4], v[5]);
      break;
    case kUnknown:
      NOTREACHED();
  }
  return value;
}

void SVGTransformValue::SetMatrix(const SVGAffineTransform& new_matrix) {
  type = kMatrix;
  angle = 0;
  matrix = new_matrix;
}

void SVGTransformValue::SetTranslate(float tx, float ty) {
  type = kTranslate;
  angle = 0;
  matrix = SVGAffineTransform();
  matrix.Translate(tx, ty);
}

void SVGTransformValue::SetScale(float sx, float sy) {
  type = kScale;
  angle = 0;
  matrix = SVGAffineTransform();
  matrix.Scale(sx, sy);
}

void SVGTransformValue::SetRotate(float new_angle, float cx, float cy) {
  type = kRotate;
  angle = new_angle;
  matrix = SVGAffineTransform();
  matrix.Translate(cx, cy);
  matrix.Rotate(new_angle);
  matrix.Translate(-cx, -cy);
}

void SVGTransformValue::SetSkewX(float new_angle) {
  type = kSkewX;
  angle = new_angle;
  matrix = SVGAffineTransform();
  matrix.SkewX(new_angle);
}

void SVGTransformValue::SetSkewY(float new_angle) {
  type = kSkewY;
  angle = new_angle;
  matrix = SVGAffineTransform();
  matrix.SkewY(new_angle);
}

std::u16string SVGTransformValue::Text() const {
  std::array<double, 6> arguments;
  size_t count = 0;
  std::u16string text;
  switch (type) {
    case kUnknown:
      return std::u16string();
    case kMatrix:
      text = u"matrix(";
      arguments = {matrix.A(), matrix.B(), matrix.C(),
                   matrix.D(), matrix.E(), matrix.F()};
      count = 6;
      break;
    case kTranslate:
      text = u"translate(";
      arguments[count++] = matrix.E();
      arguments[count++] = matrix.F();
      break;
    case kScale:
      text = u"scale(";
      arguments[count++] = matrix.A();
      arguments[count++] = matrix.D();
      break;
    case kRotate: {
      text = u"rotate(";
      arguments[count++] = angle;
      const std::array<float, 2> center = RotationCenter(matrix, angle);
      if (center[0] != 0 || center[1] != 0) {
        arguments[count++] = center[0];
        arguments[count++] = center[1];
      }
      break;
    }
    case kSkewX:
      text = u"skewX(";
      arguments[count++] = angle;
      break;
    case kSkewY:
      text = u"skewY(";
      arguments[count++] = angle;
      break;
  }
  for (size_t i = 0; i < count; ++i) {
    if (i) {
      text += u' ';
    }
    text += SVGNumberToString(arguments[i]);
  }
  text += u')';
  return text;
}

// SVGMatrix.

// static
SVGMatrix* SVGMatrix::Create(const SVGAffineTransform& value) {
  return bindings::MakeGarbageCollected<SVGMatrix>(value, nullptr);
}

// static
SVGMatrix* SVGMatrix::ForTransform(SVGTransform& transform) {
  return bindings::MakeGarbageCollected<SVGMatrix>(SVGAffineTransform(),
                                                   &transform);
}

// static
bindings::ScriptWrappable* SVGMatrix::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<SVGMatrix>(
      info, bindings::V8SVGMatrix::kWrapperTypeInfo, SVGAffineTransform(),
      nullptr);
}

SVGMatrix::SVGMatrix(const SVGAffineTransform& value, SVGTransform* transform)
    : own_value_(value), transform_(transform) {}

SVGMatrix::~SVGMatrix() = default;

const bindings::WrapperTypeInfo* SVGMatrix::GetWrapperTypeInfo() const {
  return &bindings::V8SVGMatrix::kWrapperTypeInfo;
}

void SVGMatrix::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(transform_);
}

const SVGAffineTransform& SVGMatrix::Value() const {
  return transform_ ? transform_->Value().matrix : own_value_;
}

void SVGMatrix::Set(size_t index,
                    double value,
                    bindings::ExceptionState& exception_state) {
  if (transform_ && transform_->IsReadOnly()) {
    ThrowReadOnly(exception_state);
    return;
  }
  if (!transform_) {
    own_value_.Set(index, value);
    return;
  }
  SVGAffineTransform matrix = Value();
  matrix.Set(index, value);
  transform_->MatrixChanged(matrix);
}

void SVGMatrix::SetA(double value, bindings::ExceptionState& exception_state) {
  Set(0, value, exception_state);
}

void SVGMatrix::SetB(double value, bindings::ExceptionState& exception_state) {
  Set(1, value, exception_state);
}

void SVGMatrix::SetC(double value, bindings::ExceptionState& exception_state) {
  Set(2, value, exception_state);
}

void SVGMatrix::SetD(double value, bindings::ExceptionState& exception_state) {
  Set(3, value, exception_state);
}

void SVGMatrix::SetE(double value, bindings::ExceptionState& exception_state) {
  Set(4, value, exception_state);
}

void SVGMatrix::SetF(double value, bindings::ExceptionState& exception_state) {
  Set(5, value, exception_state);
}

SVGMatrix* SVGMatrix::Multiply(SVGMatrix* second_matrix) {
  SVGAffineTransform matrix = Value();
  matrix.PreConcat(second_matrix->Value());
  return Create(matrix);
}

SVGMatrix* SVGMatrix::Inverse(bindings::ExceptionState& exception_state) {
  if (!Value().IsInvertible()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      "The matrix is not invertible.");
    return nullptr;
  }
  return Create(Value().Inverse());
}

SVGMatrix* SVGMatrix::Translate(float x, float y) {
  SVGAffineTransform matrix = Value();
  matrix.Translate(x, y);
  return Create(matrix);
}

SVGMatrix* SVGMatrix::Scale(float scale_factor) {
  SVGAffineTransform matrix = Value();
  matrix.Scale(scale_factor, scale_factor);
  return Create(matrix);
}

SVGMatrix* SVGMatrix::ScaleNonUniform(float scale_factor_x,
                                      float scale_factor_y) {
  SVGAffineTransform matrix = Value();
  matrix.Scale(scale_factor_x, scale_factor_y);
  return Create(matrix);
}

SVGMatrix* SVGMatrix::Rotate(float angle) {
  SVGAffineTransform matrix = Value();
  matrix.Rotate(angle);
  return Create(matrix);
}

SVGMatrix* SVGMatrix::RotateFromVector(
    float x,
    float y,
    bindings::ExceptionState& exception_state) {
  if (!x || !y) {
    exception_state.ThrowDOMException("InvalidAccessError",
                                      "Arguments cannot be zero.");
    return nullptr;
  }
  SVGAffineTransform matrix = Value();
  matrix.RotateFromVector(x, y);
  return Create(matrix);
}

SVGMatrix* SVGMatrix::FlipX() {
  SVGAffineTransform matrix = Value();
  matrix.Scale(-1, 1);
  return Create(matrix);
}

SVGMatrix* SVGMatrix::FlipY() {
  SVGAffineTransform matrix = Value();
  matrix.Scale(1, -1);
  return Create(matrix);
}

SVGMatrix* SVGMatrix::SkewX(float angle) {
  SVGAffineTransform matrix = Value();
  matrix.SkewX(angle);
  return Create(matrix);
}

SVGMatrix* SVGMatrix::SkewY(float angle) {
  SVGAffineTransform matrix = Value();
  matrix.SkewY(angle);
  return Create(matrix);
}

// SVGTransform.

// static
SVGTransform* SVGTransform::Create() {
  return ForItem(
      *bindings::MakeGarbageCollected<SVGListItem<SVGTransformValue>>(
          SVGTransformValue()),
      /*read_only=*/false, nullptr);
}

// static
SVGTransform* SVGTransform::CreateFromMatrix(const SVGMatrix& matrix) {
  SVGTransformValue value;
  value.SetMatrix(matrix.Value());
  return ForItem(
      *bindings::MakeGarbageCollected<SVGListItem<SVGTransformValue>>(value),
      /*read_only=*/false, nullptr);
}

// static
SVGTransform* SVGTransform::ForItem(SVGListItem<SVGTransformValue>& item,
                                    bool read_only,
                                    SVGAnimatedListBase* binding) {
  return bindings::MakeGarbageCollected<SVGTransform>(item, read_only, binding);
}

SVGTransform::SVGTransform(SVGListItem<SVGTransformValue>& item,
                           bool read_only,
                           SVGAnimatedListBase* binding)
    : item_(&item), read_only_(read_only), binding_(binding) {}

SVGTransform::~SVGTransform() = default;

const bindings::WrapperTypeInfo* SVGTransform::GetWrapperTypeInfo() const {
  return &bindings::V8SVGTransform::kWrapperTypeInfo;
}

void SVGTransform::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(item_);
  visitor->Trace(binding_);
  visitor->Trace(matrix_);
}

SVGMatrix* SVGTransform::Matrix() {
  if (!matrix_) {
    matrix_ = SVGMatrix::ForTransform(*this);
  }
  return matrix_.Get();
}

bool SVGTransform::CheckWritable(
    bindings::ExceptionState& exception_state) const {
  if (read_only_) {
    ThrowReadOnly(exception_state);
    return false;
  }
  return true;
}

void SVGTransform::Commit() {
  if (item_->owner) {
    item_->owner->ItemChanged(item_.Get());
  }
}

void SVGTransform::MatrixChanged(const SVGAffineTransform& value) {
  item_->value.SetMatrix(value);
  Commit();
}

void SVGTransform::SetMatrix(SVGMatrix* matrix,
                             bindings::ExceptionState& exception_state) {
  if (!CheckWritable(exception_state)) {
    return;
  }
  item_->value.SetMatrix(matrix->Value());
  Commit();
}

void SVGTransform::SetTranslate(float tx,
                                float ty,
                                bindings::ExceptionState& exception_state) {
  if (!CheckWritable(exception_state)) {
    return;
  }
  item_->value.SetTranslate(tx, ty);
  Commit();
}

void SVGTransform::SetScale(float sx,
                            float sy,
                            bindings::ExceptionState& exception_state) {
  if (!CheckWritable(exception_state)) {
    return;
  }
  item_->value.SetScale(sx, sy);
  Commit();
}

void SVGTransform::SetRotate(float angle,
                             float cx,
                             float cy,
                             bindings::ExceptionState& exception_state) {
  if (!CheckWritable(exception_state)) {
    return;
  }
  item_->value.SetRotate(angle, cx, cy);
  Commit();
}

void SVGTransform::SetSkewX(float angle,
                            bindings::ExceptionState& exception_state) {
  if (!CheckWritable(exception_state)) {
    return;
  }
  item_->value.SetSkewX(angle);
  Commit();
}

void SVGTransform::SetSkewY(float angle,
                            bindings::ExceptionState& exception_state) {
  if (!CheckWritable(exception_state)) {
    return;
  }
  item_->value.SetSkewY(angle);
  Commit();
}

SVGListItem<SVGTransformValue>* SVGTransform::ItemToInsert(
    SVGAnimatedListBase& list) {
  if (!read_only_ && !binding_ && !item_->owner) {
    binding_ = &list;
    return item_.Get();
  }
  return bindings::MakeGarbageCollected<SVGListItem<SVGTransformValue>>(
      item_->value);
}

// SVGAnimatedTransformList.

// static
SVGAnimatedTransformList* SVGAnimatedTransformList::Create(
    SVGElement& element,
    std::u16string_view attribute) {
  return bindings::MakeGarbageCollected<SVGAnimatedTransformList>(element,
                                                                  attribute);
}

SVGAnimatedTransformList::SVGAnimatedTransformList(
    SVGElement& element,
    std::u16string_view attribute)
    : SVGAnimatedItemList<SVGTransformValue>(element, attribute) {}

SVGAnimatedTransformList::~SVGAnimatedTransformList() = default;

const bindings::WrapperTypeInfo* SVGAnimatedTransformList::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGAnimatedTransformList::kWrapperTypeInfo;
}

void SVGAnimatedTransformList::Trace(cppgc::Visitor* visitor) const {
  SVGAnimatedItemList<SVGTransformValue>::Trace(visitor);
  visitor->Trace(base_val_);
  visitor->Trace(anim_val_);
}

SVGTransformList* SVGAnimatedTransformList::BaseVal() {
  if (!base_val_) {
    base_val_ = bindings::MakeGarbageCollected<SVGTransformList>(*this, false);
  }
  return base_val_.Get();
}

SVGTransformList* SVGAnimatedTransformList::AnimVal() {
  if (!anim_val_) {
    anim_val_ = bindings::MakeGarbageCollected<SVGTransformList>(*this, true);
  }
  return anim_val_.Get();
}

std::optional<std::vector<SVGTransformValue>>
SVGAnimatedTransformList::ParseValues(std::u16string_view text) const {
  const std::optional<std::vector<SVGTransformArguments>> parsed =
      ParseSVGTransforms(text);
  if (!parsed) {
    return std::nullopt;
  }
  std::vector<SVGTransformValue> values;
  values.reserve(parsed->size());
  for (const SVGTransformArguments& transform : *parsed) {
    values.push_back(SVGTransformValue::FromArguments(transform));
  }
  return values;
}

std::u16string SVGAnimatedTransformList::ValueText(
    const SVGTransformValue& value) const {
  return value.Text();
}

// SVGTransformList.

SVGTransformList::SVGTransformList(SVGAnimatedTransformList& owner,
                                   bool read_only)
    : SVGTearOffList<SVGAnimatedTransformList, SVGTransform, SVGTransformValue>(
          owner,
          read_only) {}

SVGTransformList::~SVGTransformList() = default;

const bindings::WrapperTypeInfo* SVGTransformList::GetWrapperTypeInfo() const {
  return &bindings::V8SVGTransformList::kWrapperTypeInfo;
}

SVGTransform* SVGTransformList::CreateSVGTransformFromMatrix(
    SVGMatrix* matrix) {
  return SVGTransform::CreateFromMatrix(*matrix);
}

SVGTransform* SVGTransformList::Consolidate(
    bindings::ExceptionState& exception_state) {
  if (ThrowIfReadOnly(exception_state)) {
    return nullptr;
  }
  SVGAnimatedTransformList& owner = OwnerList();
  const HeapVector<SVGListItem<SVGTransformValue>>& items = owner.Items();
  if (items.empty()) {
    return nullptr;
  }
  SVGAffineTransform matrix;
  for (size_t i = 0; i < items.size(); ++i) {
    matrix.PreConcat(items[i]->value.matrix);
  }
  SVGTransformValue value;
  value.SetMatrix(matrix);
  SVGListItem<SVGTransformValue>* const item =
      bindings::MakeGarbageCollected<SVGListItem<SVGTransformValue>>(value);
  owner.ResetItems(*item, /*write_attribute=*/false);
  return SVGTransform::ForItem(*item, /*read_only=*/false, &owner);
}

}  // namespace mitto
