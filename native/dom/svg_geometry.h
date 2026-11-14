// SVG's rectangles, aspect ratios, angles and points: SVGRect and
// SVGAnimatedRect for viewBox, SVGPreserveAspectRatio and its animated value,
// SVGAngle and SVGAnimatedAngle for a marker's orient, with the orientType
// that shares that attribute, and SVGPoint, SVGPointList for a polyline's or
// polygon's points. Each reads its attribute as Blink parses it and writes back
// what a page sets, as the other animated values do.
//
// Upstream: third_party/blink/renderer/core/svg/svg_rect.cc
// Upstream: third_party/blink/renderer/core/svg/svg_rect_tear_off.cc
// Upstream: third_party/blink/renderer/core/svg/svg_preserve_aspect_ratio.cc
// Upstream: third_party/blink/renderer/core/svg/svg_preserve_aspect_ratio_tear_off.cc
// Upstream: third_party/blink/renderer/core/svg/svg_angle.cc
// Upstream: third_party/blink/renderer/core/svg/svg_angle_tear_off.cc
// Upstream: third_party/blink/renderer/core/svg/svg_animated_angle.cc
// Upstream: third_party/blink/renderer/core/svg/svg_point_list.cc
// Upstream: third_party/blink/renderer/core/svg/svg_point_tear_off.cc

#ifndef MITTO_DOM_SVG_GEOMETRY_H_
#define MITTO_DOM_SVG_GEOMETRY_H_

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
class SVGMatrix;

// SVGRect.

struct SVGRectValue {
  float x = 0;
  float y = 0;
  float width = 0;
  float height = 0;
};

class SVGAnimatedRect;

class SVGRect final : public bindings::ScriptWrappable {
 public:
  // createSVGRect()'s: of no element.
  static SVGRect* Create();
  // The one a Known Gap gives, such as getBBox()'s: 0, 0, 0, 0 of no element.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);
  static SVGRect* CreateTearOff(SVGAnimatedRect& owner, bool is_anim_val);

  SVGRect(SVGAnimatedRect* owner, bool is_anim_val);
  ~SVGRect() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  float X() const { return Target().x; }
  void SetX(float value, bindings::ExceptionState& exception_state);
  float Y() const { return Target().y; }
  void SetY(float value, bindings::ExceptionState& exception_state);
  float Width() const { return Target().width; }
  void SetWidth(float value, bindings::ExceptionState& exception_state);
  float Height() const { return Target().height; }
  void SetHeight(float value, bindings::ExceptionState& exception_state);

 private:
  SVGRectValue Target() const;
  void Set(float SVGRectValue::* field,
           float value,
           bindings::ExceptionState& exception_state);

  const cppgc::Member<SVGAnimatedRect> owner_;
  const bool is_anim_val_;
  SVGRectValue own_value_;
};

class SVGAnimatedRect final : public SVGAnimatedPropertyBase {
 public:
  static SVGAnimatedRect* Create(SVGElement& element);

  explicit SVGAnimatedRect(SVGElement& element);
  ~SVGAnimatedRect() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface: the same objects on every read.
  SVGRect* BaseVal();
  SVGRect* AnimVal();

  const SVGRectValue& BaseValue();
  void SetBaseValue(const SVGRectValue& value);

 protected:
  void Parse(const std::u16string* value) override;

 private:
  SVGRectValue value_;
  cppgc::Member<SVGRect> base_val_;
  cppgc::Member<SVGRect> anim_val_;
};

// SVGPreserveAspectRatio.

struct SVGPreserveAspectRatioValue {
  // SVG_PRESERVEASPECTRATIO_XMIDYMID and SVG_MEETORSLICE_MEET.
  uint16_t align = 6;
  uint16_t meet_or_slice = 1;
};

class SVGAnimatedPreserveAspectRatio;

class SVGPreserveAspectRatio final : public bindings::ScriptWrappable {
 public:
  static SVGPreserveAspectRatio* CreateTearOff(
      SVGAnimatedPreserveAspectRatio& owner,
      bool is_anim_val);

  SVGPreserveAspectRatio(SVGAnimatedPreserveAspectRatio& owner,
                         bool is_anim_val);
  ~SVGPreserveAspectRatio() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  uint16_t Align() const;
  void SetAlign(uint16_t value, bindings::ExceptionState& exception_state);
  uint16_t MeetOrSlice() const;
  void SetMeetOrSlice(uint16_t value,
                      bindings::ExceptionState& exception_state);

 private:
  const cppgc::Member<SVGAnimatedPreserveAspectRatio> owner_;
  const bool is_anim_val_;
};

class SVGAnimatedPreserveAspectRatio final : public SVGAnimatedPropertyBase {
 public:
  static SVGAnimatedPreserveAspectRatio* Create(SVGElement& element);

  explicit SVGAnimatedPreserveAspectRatio(SVGElement& element);
  ~SVGAnimatedPreserveAspectRatio() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface: the same objects on every read.
  SVGPreserveAspectRatio* BaseVal();
  SVGPreserveAspectRatio* AnimVal();

  const SVGPreserveAspectRatioValue& BaseValue();
  void SetBaseValue(const SVGPreserveAspectRatioValue& value);

 protected:
  void Parse(const std::u16string* value) override;

 private:
  SVGPreserveAspectRatioValue value_;
  cppgc::Member<SVGPreserveAspectRatio> base_val_;
  cppgc::Member<SVGPreserveAspectRatio> anim_val_;
};

// SVGAngle.

// Blink's SVGAngle: a number in a unit, and what a marker's orient is.
struct SVGAngleValue {
  // Blink's SVGAngleType, with turn, which a page does not see, past grad.
  enum Unit : uint8_t {
    kUnknown = 0,
    kUnspecified = 1,
    kDeg = 2,
    kRad = 3,
    kGrad = 4,
    kTurn = 5,
  };
  // SVGMarkerOrientType, of which a page sees auto and angle.
  enum Orient : uint16_t {
    kAuto = 1,
    kAngle = 2,
    kAutoStartReverse = 3,
  };

  Unit unit = kUnspecified;
  float value_in_specified_units = 0;
  Orient orient = kAngle;

  // In degrees.
  float Value() const;
  void SetValue(float degrees);
  // Sets the orient to angle, as Blink's NewValueSpecifiedUnits() does.
  void NewValueSpecifiedUnits(Unit new_unit, float value);
  void ConvertToSpecifiedUnits(Unit new_unit);
  // auto and auto-start-reverse too. False for what does not parse.
  bool SetValueAsString(std::u16string_view text);
  std::u16string ValueAsString() const;
  // OrientTypeChanged(): auto and auto-start-reverse make the angle 0.
  void SetOrient(Orient new_orient);
  // What the orient attribute writes: the angle, or the orient's keyword.
  std::u16string AttributeText() const;
};

class SVGAnimatedAngle;

class SVGAngle final : public bindings::ScriptWrappable {
 public:
  // createSVGAngle()'s: of no element.
  static SVGAngle* Create();
  static SVGAngle* CreateTearOff(SVGAnimatedAngle& owner, bool is_anim_val);

  SVGAngle(SVGAnimatedAngle* owner, bool is_anim_val);
  ~SVGAngle() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface. A page sees a turn as an angle of unknown
  // unit whose string is 0.
  uint16_t UnitType() const;
  float Value() const { return Target().Value(); }
  void SetValue(float value, bindings::ExceptionState& exception_state);
  float ValueInSpecifiedUnits() const {
    return Target().value_in_specified_units;
  }
  void SetValueInSpecifiedUnits(float value,
                                bindings::ExceptionState& exception_state);
  std::u16string ValueAsString() const;
  void SetValueAsString(const std::u16string& value,
                        bindings::ExceptionState& exception_state);
  void NewValueSpecifiedUnits(uint16_t unit_type,
                              float value_in_specified_units,
                              bindings::ExceptionState& exception_state);
  void ConvertToSpecifiedUnits(uint16_t unit_type,
                               bindings::ExceptionState& exception_state);

  SVGAngleValue Target() const;

 private:
  void Commit(const SVGAngleValue& value);
  bool ThrowIfImmutable(bindings::ExceptionState& exception_state) const;

  const cppgc::Member<SVGAnimatedAngle> owner_;
  const bool is_anim_val_;
  SVGAngleValue own_value_;
};

class SVGAnimatedAngle final : public SVGAnimatedPropertyBase {
 public:
  static SVGAnimatedAngle* Create(SVGElement& element);

  explicit SVGAnimatedAngle(SVGElement& element);
  ~SVGAnimatedAngle() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface: the same objects on every read.
  SVGAngle* BaseVal();
  SVGAngle* AnimVal();

  // orientType, which reads and sets this angle's orient.
  SVGAnimatedEnumeration* OrientType();

  const SVGAngleValue& BaseValue();
  void SetBaseValue(const SVGAngleValue& value);
  // The value that SVGAngle's rollback leaves, of which the attribute knows
  // nothing.
  void SetBaseValueWithoutAttribute(const SVGAngleValue& value);

 protected:
  void Parse(const std::u16string* value) override;

 private:
  SVGAngleValue value_;
  cppgc::Member<SVGAngle> base_val_;
  cppgc::Member<SVGAngle> anim_val_;
  cppgc::Member<SVGAnimatedEnumeration> orient_type_;
};

// SVGPoint.

class SVGPoint final : public bindings::ScriptWrappable {
 public:
  // createSVGPoint()'s: 0, 0, in no list.
  static SVGPoint* Create();
  static SVGPoint* ForItem(SVGListItem<std::array<float, 2>>& item,
                           bool read_only,
                           SVGAnimatedListBase* binding);
  // The one a Known Gap gives, such as getPointAtLength()'s: a point of no
  // list, whose wrapper is of `info`.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  SVGPoint(SVGListItem<std::array<float, 2>>& item,
           bool read_only,
           SVGAnimatedListBase* binding);
  ~SVGPoint() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  float X() const { return item_->value[0]; }
  void SetX(float value, bindings::ExceptionState& exception_state);
  float Y() const { return item_->value[1]; }
  void SetY(float value, bindings::ExceptionState& exception_state);
  // A new point of no list.
  SVGPoint* MatrixTransform(SVGMatrix* matrix);

  // What a list's insertion takes, as SVGNumber's.
  SVGListItem<std::array<float, 2>>* ItemToInsert(SVGAnimatedListBase& list);

 private:
  void Set(size_t index,
           float value,
           bindings::ExceptionState& exception_state);

  const cppgc::Member<SVGListItem<std::array<float, 2>>> item_;
  const bool read_only_;
  cppgc::Member<SVGAnimatedListBase> binding_;
};

class SVGPointList;

class SVGAnimatedPointList final
    : public SVGAnimatedItemList<std::array<float, 2>> {
 public:
  static SVGAnimatedPointList* Create(SVGElement& element);

  explicit SVGAnimatedPointList(SVGElement& element);
  ~SVGAnimatedPointList() override;

  // An animated list that is no interface: its lists are points and
  // animatedPoints.
  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  SVGPointList* BaseVal();
  SVGPointList* AnimVal();

 protected:
  std::optional<std::vector<std::array<float, 2>>> ParseValues(
      std::u16string_view text) const override;
  std::u16string ValueText(const std::array<float, 2>& value) const override;

 private:
  cppgc::Member<SVGPointList> base_val_;
  cppgc::Member<SVGPointList> anim_val_;
};

class SVGPointList final : public SVGTearOffList<SVGAnimatedPointList,
                                                 SVGPoint,
                                                 std::array<float, 2>> {
 public:
  SVGPointList(SVGAnimatedPointList& owner, bool read_only);
  ~SVGPointList() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

}  // namespace mitto

#endif  // MITTO_DOM_SVG_GEOMETRY_H_
