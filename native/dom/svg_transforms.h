// SVG's transforms: SVGMatrix, SVGTransform, SVGTransformList and
// SVGAnimatedTransformList, for the transform of graphics elements and
// clipPath, gradientTransform and patternTransform. A list reads its attribute
// as Blink parses it and writes back what a page sets, as the other lists do,
// and a matrix is Blink's AffineTransform, in doubles.
//
// Upstream: third_party/blink/renderer/platform/transforms/affine_transform.cc
// Upstream: third_party/blink/renderer/core/svg/svg_matrix_tear_off.cc
// Upstream: third_party/blink/renderer/core/svg/svg_transform.cc
// Upstream: third_party/blink/renderer/core/svg/svg_transform_tear_off.cc
// Upstream: third_party/blink/renderer/core/svg/svg_transform_list.cc
// Upstream: third_party/blink/renderer/core/svg/svg_transform_list_tear_off.cc

#ifndef MITTO_DOM_SVG_TRANSFORMS_H_
#define MITTO_DOM_SVG_TRANSFORMS_H_

#include <stdint.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/svg_animated_values.h"
#include "mitto/dom/svg_lists.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class SVGElement;

// Blink's AffineTransform: a, b, c, d, e and f, in doubles.
class SVGAffineTransform {
 public:
  constexpr SVGAffineTransform() = default;
  constexpr SVGAffineTransform(double a,
                               double b,
                               double c,
                               double d,
                               double e,
                               double f)
      : values_{a, b, c, d, e, f} {}

  double A() const { return values_[0]; }
  double B() const { return values_[1]; }
  double C() const { return values_[2]; }
  double D() const { return values_[3]; }
  double E() const { return values_[4]; }
  double F() const { return values_[5]; }
  // 0 for a, up to 5 for f.
  void Set(size_t index, double value) { values_[index] = value; }

  bool IsIdentityOrTranslation() const;
  bool IsInvertible() const;
  SVGAffineTransform Inverse() const;
  // this = this * other.
  SVGAffineTransform& PreConcat(const SVGAffineTransform& other);
  // Each in degrees.
  SVGAffineTransform& Rotate(double angle);
  SVGAffineTransform& RotateRadians(double angle);
  SVGAffineTransform& Scale(double sx, double sy);
  SVGAffineTransform& Translate(double tx, double ty);
  SVGAffineTransform& RotateFromVector(double x, double y);
  SVGAffineTransform& SkewX(double angle);
  SVGAffineTransform& SkewY(double angle);
  // A point through the matrix, each coordinate clamped to a float.
  std::array<float, 2> MapPoint(const std::array<float, 2>& point) const;

 private:
  SVGAffineTransform& Shear(double sx, double sy);

  std::array<double, 6> values_ = {1, 0, 0, 1, 0, 0};
};

// Blink's SVGTransform: a type of SVGTransformType, the angle of rotate,
// skewX and skewY, and the matrix.
struct SVGTransformValue {
  enum Type : uint8_t {
    kUnknown = 0,
    kMatrix = 1,
    kTranslate = 2,
    kScale = 3,
    kRotate = 4,
    kSkewX = 5,
    kSkewY = 6,
  };

  // CreateTransformFromValues() of a parsed function.
  static SVGTransformValue FromArguments(const SVGTransformArguments& parsed);

  // OnMatrixChange(): a matrix, of no angle.
  void SetMatrix(const SVGAffineTransform& new_matrix);
  void SetTranslate(float tx, float ty);
  void SetScale(float sx, float sy);
  void SetRotate(float new_angle, float cx, float cy);
  void SetSkewX(float new_angle);
  void SetSkewY(float new_angle);
  // ValueAsString(): the function and its arguments, each to six significant
  // digits, a rotation's center found again from its matrix.
  std::u16string Text() const;

  Type type = kMatrix;
  float angle = 0;
  SVGAffineTransform matrix;
};

class SVGTransform;

class SVGMatrix final : public bindings::ScriptWrappable {
 public:
  // createSVGMatrix()'s, and each that a method gives: of no transform.
  static SVGMatrix* Create(const SVGAffineTransform& value);
  // A transform's matrix, which reads and writes the transform's.
  static SVGMatrix* ForTransform(SVGTransform& transform);
  // The one a Known Gap gives, such as getCTM()'s: the identity.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  SVGMatrix(const SVGAffineTransform& value, SVGTransform* transform);
  ~SVGMatrix() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  double A() const { return Value().A(); }
  void SetA(double value, bindings::ExceptionState& exception_state);
  double B() const { return Value().B(); }
  void SetB(double value, bindings::ExceptionState& exception_state);
  double C() const { return Value().C(); }
  void SetC(double value, bindings::ExceptionState& exception_state);
  double D() const { return Value().D(); }
  void SetD(double value, bindings::ExceptionState& exception_state);
  double E() const { return Value().E(); }
  void SetE(double value, bindings::ExceptionState& exception_state);
  double F() const { return Value().F(); }
  void SetF(double value, bindings::ExceptionState& exception_state);
  SVGMatrix* Multiply(SVGMatrix* second_matrix);
  SVGMatrix* Inverse(bindings::ExceptionState& exception_state);
  SVGMatrix* Translate(float x, float y);
  SVGMatrix* Scale(float scale_factor);
  SVGMatrix* ScaleNonUniform(float scale_factor_x, float scale_factor_y);
  SVGMatrix* Rotate(float angle);
  SVGMatrix* RotateFromVector(float x,
                              float y,
                              bindings::ExceptionState& exception_state);
  SVGMatrix* FlipX();
  SVGMatrix* FlipY();
  SVGMatrix* SkewX(float angle);
  SVGMatrix* SkewY(float angle);

  const SVGAffineTransform& Value() const;

 private:
  void Set(size_t index,
           double value,
           bindings::ExceptionState& exception_state);

  SVGAffineTransform own_value_;
  const cppgc::Member<SVGTransform> transform_;
};

class SVGTransform final : public bindings::ScriptWrappable {
 public:
  // createSVGTransform()'s: the identity matrix, in no list.
  static SVGTransform* Create();
  // createSVGTransformFromMatrix()'s: a copy of `matrix`, in no list.
  static SVGTransform* CreateFromMatrix(const SVGMatrix& matrix);
  static SVGTransform* ForItem(SVGListItem<SVGTransformValue>& item,
                               bool read_only,
                               SVGAnimatedListBase* binding);

  SVGTransform(SVGListItem<SVGTransformValue>& item,
               bool read_only,
               SVGAnimatedListBase* binding);
  ~SVGTransform() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  uint16_t TransformType() const { return item_->value.type; }
  // The same object on every read, as Blink's tear-off keeps one.
  SVGMatrix* Matrix();
  float Angle() const { return item_->value.angle; }
  void SetMatrix(SVGMatrix* matrix, bindings::ExceptionState& exception_state);
  void SetTranslate(float tx,
                    float ty,
                    bindings::ExceptionState& exception_state);
  void SetScale(float sx, float sy, bindings::ExceptionState& exception_state);
  void SetRotate(float angle,
                 float cx,
                 float cy,
                 bindings::ExceptionState& exception_state);
  void SetSkewX(float angle, bindings::ExceptionState& exception_state);
  void SetSkewY(float angle, bindings::ExceptionState& exception_state);

  const SVGTransformValue& Value() const { return item_->value; }
  bool IsReadOnly() const { return read_only_; }
  // What its matrix changed: the transform is a matrix now, which its list
  // writes.
  void MatrixChanged(const SVGAffineTransform& value);
  // What a list's insertion takes, as SVGNumber's.
  SVGListItem<SVGTransformValue>* ItemToInsert(SVGAnimatedListBase& list);

 private:
  // False, after throwing, for an animVal's.
  bool CheckWritable(bindings::ExceptionState& exception_state) const;
  void Commit();

  const cppgc::Member<SVGListItem<SVGTransformValue>> item_;
  const bool read_only_;
  cppgc::Member<SVGAnimatedListBase> binding_;
  cppgc::Member<SVGMatrix> matrix_;
};

class SVGTransformList;

class SVGAnimatedTransformList final
    : public SVGAnimatedItemList<SVGTransformValue> {
 public:
  static SVGAnimatedTransformList* Create(SVGElement& element,
                                          std::u16string_view attribute);

  SVGAnimatedTransformList(SVGElement& element, std::u16string_view attribute);
  ~SVGAnimatedTransformList() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface: the same objects on every read.
  SVGTransformList* BaseVal();
  SVGTransformList* AnimVal();

 protected:
  std::optional<std::vector<SVGTransformValue>> ParseValues(
      std::u16string_view text) const override;
  std::u16string ValueText(const SVGTransformValue& value) const override;

 private:
  cppgc::Member<SVGTransformList> base_val_;
  cppgc::Member<SVGTransformList> anim_val_;
};

class SVGTransformList final : public SVGTearOffList<SVGAnimatedTransformList,
                                                     SVGTransform,
                                                     SVGTransformValue> {
 public:
  SVGTransformList(SVGAnimatedTransformList& owner, bool read_only);
  ~SVGTransformList() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface that the other lists lack.
  SVGTransform* CreateSVGTransformFromMatrix(SVGMatrix* matrix);
  // The matrices multiplied into one transform, which the list holds alone
  // without writing its attribute, as Blink commits no change of it; or null
  // for an empty list.
  SVGTransform* Consolidate(bindings::ExceptionState& exception_state);
};

}  // namespace mitto

#endif  // MITTO_DOM_SVG_TRANSFORMS_H_
