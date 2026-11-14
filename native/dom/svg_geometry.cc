#include "mitto/dom/svg_geometry.h"

#include <array>
#include <numbers>
#include <utility>

#include "base/notreached.h"
#include "base/numerics/angle_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_svg_angle.h"
#include "mitto/bindings/v8_svg_animated_angle.h"
#include "mitto/bindings/v8_svg_animated_preserve_aspect_ratio.h"
#include "mitto/bindings/v8_svg_animated_rect.h"
#include "mitto/bindings/v8_svg_point.h"
#include "mitto/bindings/v8_svg_point_list.h"
#include "mitto/bindings/v8_svg_preserve_aspect_ratio.h"
#include "mitto/bindings/v8_svg_rect.h"
#include "mitto/dom/svg_element.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

namespace {

void ThrowReadOnly(bindings::ExceptionState& exception_state) {
  exception_state.ThrowDOMException("NoModificationAllowedError",
                                    "This object is read-only.");
}

void ThrowInvalidAngle(const std::u16string& value,
                       bindings::ExceptionState& exception_state) {
  exception_state.ThrowDOMException(
      "SyntaxError",
      base::StrCat({"The value provided ('", base::UTF16ToUTF8(value),
                    "') is invalid."}));
}

// WTF's float conversions of angles.
float Grad2deg(float g) {
  return g * (360.0f / 400.0f);
}
float Deg2grad(float d) {
  return d * (400.0f / 360.0f);
}
float Turn2deg(float t) {
  return t * 360.0f;
}
float Deg2turn(float d) {
  return d * (1.0f / 360.0f);
}
float Rad2grad(float r) {
  return r * (200.0f / std::numbers::pi_v<float>);
}
float Grad2rad(float g) {
  return g * (std::numbers::pi_v<float> / 200.0f);
}
float Turn2grad(float t) {
  return t * 400;
}
float Grad2turn(float g) {
  return g * (1.0f / 400.0f);
}

using Unit = SVGAngleValue::Unit;

// Blink's ConvertAngleToUnit().
float ConvertAngleToUnit(Unit from, float value, Unit to) {
  switch (from) {
    case Unit::kTurn:
      switch (to) {
        case Unit::kGrad:
          return Turn2grad(value);
        case Unit::kUnspecified:
        case Unit::kDeg:
          return Turn2deg(value);
        case Unit::kRad:
          return base::DegToRad(Turn2deg(value));
        case Unit::kTurn:
        case Unit::kUnknown:
          return value;
      }
      break;
    case Unit::kRad:
      switch (to) {
        case Unit::kGrad:
          return Rad2grad(value);
        case Unit::kUnspecified:
        case Unit::kDeg:
          return base::RadToDeg(value);
        case Unit::kTurn:
          return Deg2turn(base::RadToDeg(value));
        case Unit::kRad:
        case Unit::kUnknown:
          return value;
      }
      break;
    case Unit::kGrad:
      switch (to) {
        case Unit::kRad:
          return Grad2rad(value);
        case Unit::kUnspecified:
        case Unit::kDeg:
          return Grad2deg(value);
        case Unit::kTurn:
          return Grad2turn(value);
        case Unit::kGrad:
        case Unit::kUnknown:
          return value;
      }
      break;
    case Unit::kUnspecified:
    case Unit::kDeg:
      switch (to) {
        case Unit::kRad:
          return base::DegToRad(value);
        case Unit::kGrad:
          return Deg2grad(value);
        case Unit::kTurn:
          return Deg2turn(value);
        case Unit::kUnspecified:
        case Unit::kDeg:
        case Unit::kUnknown:
          return value;
      }
      break;
    case Unit::kUnknown:
      return value;
  }
  NOTREACHED();
}

std::u16string RectText(const SVGRectValue& value) {
  return base::StrCat(
      {SVGNumberToString(value.x), u" ", SVGNumberToString(value.y), u" ",
       SVGNumberToString(value.width), u" ", SVGNumberToString(value.height)});
}

constexpr std::array<std::u16string_view, 11> kAlignNames = {
    u"unknown",  u"none",     u"xMinYMin", u"xMidYMin",
    u"xMaxYMin", u"xMinYMid", u"xMidYMid", u"xMaxYMid",
    u"xMinYMax", u"xMidYMax", u"xMaxYMax",
};

// SVGPreserveAspectRatio::ParseInternal(): none or an alignment, in its case,
// then meet or slice, which none ignores, then nothing but spaces.
std::optional<SVGPreserveAspectRatioValue> ParsePreserveAspectRatio(
    std::u16string_view text) {
  auto is_space = [](char16_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
  };
  auto skip_spaces = [&]() {
    while (!text.empty() && is_space(text.front())) {
      text.remove_prefix(1);
    }
  };
  SVGPreserveAspectRatioValue value;
  skip_spaces();
  if (text.empty()) {
    return std::nullopt;
  }
  if (text.front() == 'n') {
    if (text.substr(0, 4) != u"none") {
      return std::nullopt;
    }
    value.align = 1;
    text.remove_prefix(4);
  } else if (text.front() == 'x') {
    if (text.size() < 8) {
      return std::nullopt;
    }
    bool found = false;
    for (uint16_t align = 2; align <= 10; ++align) {
      if (text.substr(0, 8) == kAlignNames[align]) {
        value.align = align;
        found = true;
        break;
      }
    }
    if (!found) {
      return std::nullopt;
    }
    text.remove_prefix(8);
  } else {
    return std::nullopt;
  }
  skip_spaces();
  if (!text.empty()) {
    if (text.front() == 'm') {
      if (text.substr(0, 4) != u"meet") {
        return std::nullopt;
      }
      text.remove_prefix(4);
      skip_spaces();
    } else if (text.front() == 's') {
      if (text.substr(0, 5) != u"slice") {
        return std::nullopt;
      }
      text.remove_prefix(5);
      skip_spaces();
      if (value.align != 1) {
        value.meet_or_slice = 2;
      }
    }
  }
  if (!text.empty()) {
    return std::nullopt;
  }
  return value;
}

std::u16string PreserveAspectRatioText(
    const SVGPreserveAspectRatioValue& value) {
  std::u16string text(kAlignNames[value.align]);
  if (value.meet_or_slice == 1) {
    text += u" meet";
  } else if (value.meet_or_slice == 2) {
    text += u" slice";
  }
  return text;
}

}  // namespace

// SVGRect.

// static
SVGRect* SVGRect::Create() {
  return bindings::MakeGarbageCollected<SVGRect>(nullptr, false);
}

// static
bindings::ScriptWrappable* SVGRect::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<SVGRect>(
      info, bindings::V8SVGRect::kWrapperTypeInfo, nullptr, false);
}

// static
SVGRect* SVGRect::CreateTearOff(SVGAnimatedRect& owner, bool is_anim_val) {
  return bindings::MakeGarbageCollected<SVGRect>(&owner, is_anim_val);
}

SVGRect::SVGRect(SVGAnimatedRect* owner, bool is_anim_val)
    : owner_(owner), is_anim_val_(is_anim_val) {}

SVGRect::~SVGRect() = default;

const bindings::WrapperTypeInfo* SVGRect::GetWrapperTypeInfo() const {
  return &bindings::V8SVGRect::kWrapperTypeInfo;
}

void SVGRect::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(owner_);
}

SVGRectValue SVGRect::Target() const {
  return owner_ ? owner_->BaseValue() : own_value_;
}

void SVGRect::Set(float SVGRectValue::* field,
                  float value,
                  bindings::ExceptionState& exception_state) {
  if (is_anim_val_) {
    ThrowReadOnly(exception_state);
    return;
  }
  SVGRectValue target = Target();
  target.*field = value;
  if (owner_) {
    owner_->SetBaseValue(target);
  } else {
    own_value_ = target;
  }
}

void SVGRect::SetX(float value, bindings::ExceptionState& exception_state) {
  Set(&SVGRectValue::x, value, exception_state);
}

void SVGRect::SetY(float value, bindings::ExceptionState& exception_state) {
  Set(&SVGRectValue::y, value, exception_state);
}

void SVGRect::SetWidth(float value, bindings::ExceptionState& exception_state) {
  Set(&SVGRectValue::width, value, exception_state);
}

void SVGRect::SetHeight(float value,
                        bindings::ExceptionState& exception_state) {
  Set(&SVGRectValue::height, value, exception_state);
}

// static
SVGAnimatedRect* SVGAnimatedRect::Create(SVGElement& element) {
  return bindings::MakeGarbageCollected<SVGAnimatedRect>(element);
}

SVGAnimatedRect::SVGAnimatedRect(SVGElement& element)
    : SVGAnimatedPropertyBase(element, u"viewBox", SVGPairHalf::kNone) {}

SVGAnimatedRect::~SVGAnimatedRect() = default;

const bindings::WrapperTypeInfo* SVGAnimatedRect::GetWrapperTypeInfo() const {
  return &bindings::V8SVGAnimatedRect::kWrapperTypeInfo;
}

void SVGAnimatedRect::Trace(cppgc::Visitor* visitor) const {
  SVGAnimatedPropertyBase::Trace(visitor);
  visitor->Trace(base_val_);
  visitor->Trace(anim_val_);
}

SVGRect* SVGAnimatedRect::BaseVal() {
  if (!base_val_) {
    base_val_ = SVGRect::CreateTearOff(*this, /*is_anim_val=*/false);
  }
  return base_val_.Get();
}

SVGRect* SVGAnimatedRect::AnimVal() {
  if (!anim_val_) {
    anim_val_ = SVGRect::CreateTearOff(*this, /*is_anim_val=*/true);
  }
  return anim_val_.Get();
}

const SVGRectValue& SVGAnimatedRect::BaseValue() {
  EnsureFresh();
  return value_;
}

void SVGAnimatedRect::SetBaseValue(const SVGRectValue& value) {
  EnsureFresh();
  value_ = value;
  SynchronizeAttribute(RectText(value_));
}

// A viewBox that does not parse is 0 0 0 0. A negative size parses: only
// rendering refuses it.
void SVGAnimatedRect::Parse(const std::u16string* value) {
  value_ = SVGRectValue();
  if (!value) {
    return;
  }
  if (const std::optional<std::array<float, 4>> numbers =
          ParseSVGRect(*value)) {
    value_ = {(*numbers)[0], (*numbers)[1], (*numbers)[2], (*numbers)[3]};
  }
}

// SVGPreserveAspectRatio.

// static
SVGPreserveAspectRatio* SVGPreserveAspectRatio::CreateTearOff(
    SVGAnimatedPreserveAspectRatio& owner,
    bool is_anim_val) {
  return bindings::MakeGarbageCollected<SVGPreserveAspectRatio>(owner,
                                                                is_anim_val);
}

SVGPreserveAspectRatio::SVGPreserveAspectRatio(
    SVGAnimatedPreserveAspectRatio& owner,
    bool is_anim_val)
    : owner_(&owner), is_anim_val_(is_anim_val) {}

SVGPreserveAspectRatio::~SVGPreserveAspectRatio() = default;

const bindings::WrapperTypeInfo* SVGPreserveAspectRatio::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGPreserveAspectRatio::kWrapperTypeInfo;
}

void SVGPreserveAspectRatio::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(owner_);
}

uint16_t SVGPreserveAspectRatio::Align() const {
  return owner_->BaseValue().align;
}

uint16_t SVGPreserveAspectRatio::MeetOrSlice() const {
  return owner_->BaseValue().meet_or_slice;
}

void SVGPreserveAspectRatio::SetAlign(
    uint16_t value,
    bindings::ExceptionState& exception_state) {
  if (is_anim_val_) {
    ThrowReadOnly(exception_state);
    return;
  }
  if (value < 1 || value > 10) {
    exception_state.ThrowDOMException("NotSupportedError",
                                      "The alignment provided is invalid.");
    return;
  }
  SVGPreserveAspectRatioValue target = owner_->BaseValue();
  target.align = value;
  owner_->SetBaseValue(target);
}

void SVGPreserveAspectRatio::SetMeetOrSlice(
    uint16_t value,
    bindings::ExceptionState& exception_state) {
  if (is_anim_val_) {
    ThrowReadOnly(exception_state);
    return;
  }
  if (value < 1 || value > 2) {
    exception_state.ThrowDOMException("NotSupportedError",
                                      "The meetOrSlice provided is invalid.");
    return;
  }
  SVGPreserveAspectRatioValue target = owner_->BaseValue();
  target.meet_or_slice = value;
  owner_->SetBaseValue(target);
}

// static
SVGAnimatedPreserveAspectRatio* SVGAnimatedPreserveAspectRatio::Create(
    SVGElement& element) {
  return bindings::MakeGarbageCollected<SVGAnimatedPreserveAspectRatio>(
      element);
}

SVGAnimatedPreserveAspectRatio::SVGAnimatedPreserveAspectRatio(
    SVGElement& element)
    : SVGAnimatedPropertyBase(element,
                              u"preserveAspectRatio",
                              SVGPairHalf::kNone) {}

SVGAnimatedPreserveAspectRatio::~SVGAnimatedPreserveAspectRatio() = default;

const bindings::WrapperTypeInfo*
SVGAnimatedPreserveAspectRatio::GetWrapperTypeInfo() const {
  return &bindings::V8SVGAnimatedPreserveAspectRatio::kWrapperTypeInfo;
}

void SVGAnimatedPreserveAspectRatio::Trace(cppgc::Visitor* visitor) const {
  SVGAnimatedPropertyBase::Trace(visitor);
  visitor->Trace(base_val_);
  visitor->Trace(anim_val_);
}

SVGPreserveAspectRatio* SVGAnimatedPreserveAspectRatio::BaseVal() {
  if (!base_val_) {
    base_val_ = SVGPreserveAspectRatio::CreateTearOff(*this, false);
  }
  return base_val_.Get();
}

SVGPreserveAspectRatio* SVGAnimatedPreserveAspectRatio::AnimVal() {
  if (!anim_val_) {
    anim_val_ = SVGPreserveAspectRatio::CreateTearOff(*this, true);
  }
  return anim_val_.Get();
}

const SVGPreserveAspectRatioValue& SVGAnimatedPreserveAspectRatio::BaseValue() {
  EnsureFresh();
  return value_;
}

void SVGAnimatedPreserveAspectRatio::SetBaseValue(
    const SVGPreserveAspectRatioValue& value) {
  EnsureFresh();
  value_ = value;
  SynchronizeAttribute(PreserveAspectRatioText(value_));
}

void SVGAnimatedPreserveAspectRatio::Parse(const std::u16string* value) {
  value_ = SVGPreserveAspectRatioValue();
  if (!value) {
    return;
  }
  value_ = ParsePreserveAspectRatio(*value).value_or(value_);
}

// SVGAngleValue.

float SVGAngleValue::Value() const {
  switch (unit) {
    case Unit::kGrad:
      return Grad2deg(value_in_specified_units);
    case Unit::kRad:
      return base::RadToDeg(value_in_specified_units);
    case Unit::kTurn:
      return Turn2deg(value_in_specified_units);
    case Unit::kUnspecified:
    case Unit::kUnknown:
    case Unit::kDeg:
      return value_in_specified_units;
  }
  NOTREACHED();
}

void SVGAngleValue::SetValue(float degrees) {
  float converted = degrees;
  switch (unit) {
    case Unit::kGrad:
      converted = Deg2grad(degrees);
      break;
    case Unit::kRad:
      converted = base::DegToRad(degrees);
      break;
    case Unit::kTurn:
      converted = Deg2turn(degrees);
      break;
    case Unit::kUnspecified:
    case Unit::kUnknown:
    case Unit::kDeg:
      break;
  }
  NewValueSpecifiedUnits(unit, converted);
}

void SVGAngleValue::NewValueSpecifiedUnits(Unit new_unit, float value) {
  orient = kAngle;
  unit = new_unit;
  value_in_specified_units = value;
}

void SVGAngleValue::ConvertToSpecifiedUnits(Unit new_unit) {
  // The same unit changes nothing of a numeric angle, but makes an angle of
  // an orient that is not one.
  if (orient == kAngle && new_unit == unit) {
    return;
  }
  NewValueSpecifiedUnits(
      new_unit, ConvertAngleToUnit(unit, value_in_specified_units, new_unit));
}

bool SVGAngleValue::SetValueAsString(std::u16string_view text) {
  if (text == u"auto") {
    NewValueSpecifiedUnits(kUnspecified, 0);
    SetOrient(kAuto);
    return true;
  }
  if (text == u"auto-start-reverse") {
    NewValueSpecifiedUnits(kUnspecified, 0);
    SetOrient(kAutoStartReverse);
    return true;
  }
  const std::optional<std::pair<float, uint8_t>> parsed = ParseSVGAngle(text);
  if (!parsed) {
    return false;
  }
  NewValueSpecifiedUnits(static_cast<Unit>(parsed->second), parsed->first);
  return true;
}

std::u16string SVGAngleValue::ValueAsString() const {
  std::u16string_view unit_name;
  switch (unit) {
    case Unit::kDeg:
      unit_name = u"deg";
      break;
    case Unit::kRad:
      unit_name = u"rad";
      break;
    case Unit::kGrad:
      unit_name = u"grad";
      break;
    case Unit::kTurn:
      unit_name = u"turn";
      break;
    case Unit::kUnspecified:
    case Unit::kUnknown:
      break;
  }
  return base::StrCat({SVGNumberToString(value_in_specified_units), unit_name});
}

void SVGAngleValue::SetOrient(Orient new_orient) {
  orient = new_orient;
  if (orient == kAuto || orient == kAutoStartReverse) {
    unit = kUnspecified;
    value_in_specified_units = 0;
  }
}

// SVGAnimatedAngle::SynchronizeAttribute(): the enumeration's keyword for an
// orient that is not an angle.
std::u16string SVGAngleValue::AttributeText() const {
  switch (orient) {
    case kAuto:
      return u"auto";
    case kAutoStartReverse:
      return u"auto-start-reverse";
    case kAngle:
      return ValueAsString();
  }
  NOTREACHED();
}

// SVGAngle.

// static
SVGAngle* SVGAngle::Create() {
  return bindings::MakeGarbageCollected<SVGAngle>(nullptr, false);
}

// static
SVGAngle* SVGAngle::CreateTearOff(SVGAnimatedAngle& owner, bool is_anim_val) {
  return bindings::MakeGarbageCollected<SVGAngle>(&owner, is_anim_val);
}

SVGAngle::SVGAngle(SVGAnimatedAngle* owner, bool is_anim_val)
    : owner_(owner), is_anim_val_(is_anim_val) {}

SVGAngle::~SVGAngle() = default;

const bindings::WrapperTypeInfo* SVGAngle::GetWrapperTypeInfo() const {
  return &bindings::V8SVGAngle::kWrapperTypeInfo;
}

void SVGAngle::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(owner_);
}

SVGAngleValue SVGAngle::Target() const {
  return owner_ ? owner_->BaseValue() : own_value_;
}

void SVGAngle::Commit(const SVGAngleValue& value) {
  if (owner_) {
    owner_->SetBaseValue(value);
  } else {
    own_value_ = value;
  }
}

bool SVGAngle::ThrowIfImmutable(
    bindings::ExceptionState& exception_state) const {
  if (is_anim_val_) {
    ThrowReadOnly(exception_state);
  }
  return is_anim_val_;
}

// HasExposedAngleUnit(): turn is past grad.
uint16_t SVGAngle::UnitType() const {
  const Unit unit = Target().unit;
  return unit <= Unit::kGrad ? unit : Unit::kUnknown;
}

std::u16string SVGAngle::ValueAsString() const {
  const SVGAngleValue target = Target();
  return target.unit <= Unit::kGrad ? target.ValueAsString() : u"0";
}

void SVGAngle::SetValue(float value,
                        bindings::ExceptionState& exception_state) {
  if (ThrowIfImmutable(exception_state)) {
    return;
  }
  SVGAngleValue target = Target();
  target.SetValue(value);
  Commit(target);
}

void SVGAngle::SetValueInSpecifiedUnits(
    float value,
    bindings::ExceptionState& exception_state) {
  if (ThrowIfImmutable(exception_state)) {
    return;
  }
  SVGAngleValue target = Target();
  target.NewValueSpecifiedUnits(target.unit, value);
  Commit(target);
}

// A turn parses, but a page cannot set one, so Blink parses the old value's
// string again, which keeps 6 digits and makes the orient angle, and writes no
// attribute.
void SVGAngle::SetValueAsString(const std::u16string& value,
                                bindings::ExceptionState& exception_state) {
  if (ThrowIfImmutable(exception_state)) {
    return;
  }
  SVGAngleValue target = Target();
  const std::u16string old_value = target.ValueAsString();
  if (!target.SetValueAsString(value)) {
    ThrowInvalidAngle(value, exception_state);
    return;
  }
  if (target.unit > Unit::kGrad) {
    target.SetValueAsString(old_value);
    if (owner_) {
      owner_->SetBaseValueWithoutAttribute(target);
    } else {
      own_value_ = target;
    }
    ThrowInvalidAngle(value, exception_state);
    return;
  }
  Commit(target);
}

void SVGAngle::NewValueSpecifiedUnits(
    uint16_t unit_type,
    float value_in_specified_units,
    bindings::ExceptionState& exception_state) {
  if (ThrowIfImmutable(exception_state)) {
    return;
  }
  if (unit_type == Unit::kUnknown || unit_type > Unit::kGrad) {
    exception_state.ThrowDOMException(
        "NotSupportedError",
        base::StrCat({"Cannot set value with unknown or invalid units (",
                      base::NumberToString(unit_type), ")."}));
    return;
  }
  SVGAngleValue target = Target();
  target.NewValueSpecifiedUnits(static_cast<Unit>(unit_type),
                                value_in_specified_units);
  Commit(target);
}

void SVGAngle::ConvertToSpecifiedUnits(
    uint16_t unit_type,
    bindings::ExceptionState& exception_state) {
  if (ThrowIfImmutable(exception_state)) {
    return;
  }
  if (unit_type == Unit::kUnknown || unit_type > Unit::kGrad) {
    exception_state.ThrowDOMException(
        "NotSupportedError",
        base::StrCat({"Cannot convert to unknown or invalid units (",
                      base::NumberToString(unit_type), ")."}));
    return;
  }
  SVGAngleValue target = Target();
  if (target.unit == Unit::kUnknown) {
    exception_state.ThrowDOMException(
        "NotSupportedError", "Cannot convert from unknown or invalid units.");
    return;
  }
  target.ConvertToSpecifiedUnits(static_cast<Unit>(unit_type));
  Commit(target);
}

// SVGAnimatedAngle.

// static
SVGAnimatedAngle* SVGAnimatedAngle::Create(SVGElement& element) {
  return bindings::MakeGarbageCollected<SVGAnimatedAngle>(element);
}

SVGAnimatedAngle::SVGAnimatedAngle(SVGElement& element)
    : SVGAnimatedPropertyBase(element, u"orient", SVGPairHalf::kNone) {}

SVGAnimatedAngle::~SVGAnimatedAngle() = default;

const bindings::WrapperTypeInfo* SVGAnimatedAngle::GetWrapperTypeInfo() const {
  return &bindings::V8SVGAnimatedAngle::kWrapperTypeInfo;
}

void SVGAnimatedAngle::Trace(cppgc::Visitor* visitor) const {
  SVGAnimatedPropertyBase::Trace(visitor);
  visitor->Trace(base_val_);
  visitor->Trace(anim_val_);
  visitor->Trace(orient_type_);
}

SVGAngle* SVGAnimatedAngle::BaseVal() {
  if (!base_val_) {
    base_val_ = SVGAngle::CreateTearOff(*this, /*is_anim_val=*/false);
  }
  return base_val_.Get();
}

SVGAngle* SVGAnimatedAngle::AnimVal() {
  if (!anim_val_) {
    anim_val_ = SVGAngle::CreateTearOff(*this, /*is_anim_val=*/true);
  }
  return anim_val_.Get();
}

SVGAnimatedEnumeration* SVGAnimatedAngle::OrientType() {
  if (!orient_type_) {
    orient_type_ =
        SVGAnimatedEnumeration::CreateForAngle(ContextElement(), *this);
  }
  return orient_type_.Get();
}

const SVGAngleValue& SVGAnimatedAngle::BaseValue() {
  EnsureFresh();
  return value_;
}

void SVGAnimatedAngle::SetBaseValue(const SVGAngleValue& value) {
  EnsureFresh();
  value_ = value;
  SynchronizeAttribute(value_.AttributeText());
}

void SVGAnimatedAngle::SetBaseValueWithoutAttribute(
    const SVGAngleValue& value) {
  EnsureFresh();
  value_ = value;
}

// What does not parse is the initial angle of 0, of no unit.
void SVGAnimatedAngle::Parse(const std::u16string* value) {
  value_ = SVGAngleValue();
  if (value && !value_.SetValueAsString(*value)) {
    value_ = SVGAngleValue();
  }
}

// SVGPoint.

// static
SVGPoint* SVGPoint::Create() {
  return bindings::MakeGarbageCollected<SVGPoint>(
      *bindings::MakeGarbageCollected<SVGListItem<std::array<float, 2>>>(
          std::array<float, 2>{0, 0}),
      false, nullptr);
}

// static
SVGPoint* SVGPoint::ForItem(SVGListItem<std::array<float, 2>>& item,
                            bool read_only,
                            SVGAnimatedListBase* binding) {
  return bindings::MakeGarbageCollected<SVGPoint>(item, read_only, binding);
}

// static
bindings::ScriptWrappable* SVGPoint::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<SVGPoint>(
      info, bindings::V8SVGPoint::kWrapperTypeInfo,
      *bindings::MakeGarbageCollected<SVGListItem<std::array<float, 2>>>(
          std::array<float, 2>{0, 0}),
      false, nullptr);
}

SVGPoint::SVGPoint(SVGListItem<std::array<float, 2>>& item,
                   bool read_only,
                   SVGAnimatedListBase* binding)
    : item_(&item), read_only_(read_only), binding_(binding) {}

SVGPoint::~SVGPoint() = default;

const bindings::WrapperTypeInfo* SVGPoint::GetWrapperTypeInfo() const {
  return &bindings::V8SVGPoint::kWrapperTypeInfo;
}

void SVGPoint::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(item_);
  visitor->Trace(binding_);
}

void SVGPoint::Set(size_t index,
                   float value,
                   bindings::ExceptionState& exception_state) {
  if (read_only_) {
    ThrowReadOnly(exception_state);
    return;
  }
  item_->value[index] = value;
  if (item_->owner) {
    item_->owner->ItemChanged(item_.Get());
  }
}

void SVGPoint::SetX(float value, bindings::ExceptionState& exception_state) {
  Set(0, value, exception_state);
}

void SVGPoint::SetY(float value, bindings::ExceptionState& exception_state) {
  Set(1, value, exception_state);
}

SVGListItem<std::array<float, 2>>* SVGPoint::ItemToInsert(
    SVGAnimatedListBase& list) {
  if (!read_only_ && !binding_ && !item_->owner) {
    binding_ = &list;
    return item_.Get();
  }
  return bindings::MakeGarbageCollected<SVGListItem<std::array<float, 2>>>(
      item_->value);
}

// SVGAnimatedPointList.

// static
SVGAnimatedPointList* SVGAnimatedPointList::Create(SVGElement& element) {
  return bindings::MakeGarbageCollected<SVGAnimatedPointList>(element);
}

SVGAnimatedPointList::SVGAnimatedPointList(SVGElement& element)
    : SVGAnimatedItemList<std::array<float, 2>>(element, u"points") {}

SVGAnimatedPointList::~SVGAnimatedPointList() = default;

const bindings::WrapperTypeInfo* SVGAnimatedPointList::GetWrapperTypeInfo()
    const {
  // No interface: a page reaches only its lists.
  return nullptr;
}

void SVGAnimatedPointList::Trace(cppgc::Visitor* visitor) const {
  SVGAnimatedItemList<std::array<float, 2>>::Trace(visitor);
  visitor->Trace(base_val_);
  visitor->Trace(anim_val_);
}

SVGPointList* SVGAnimatedPointList::BaseVal() {
  if (!base_val_) {
    base_val_ = bindings::MakeGarbageCollected<SVGPointList>(*this, false);
  }
  return base_val_.Get();
}

SVGPointList* SVGAnimatedPointList::AnimVal() {
  if (!anim_val_) {
    anim_val_ = bindings::MakeGarbageCollected<SVGPointList>(*this, true);
  }
  return anim_val_.Get();
}

std::optional<std::vector<std::array<float, 2>>>
SVGAnimatedPointList::ParseValues(std::u16string_view text) const {
  return ParseSVGPoints(text);
}

std::u16string SVGAnimatedPointList::ValueText(
    const std::array<float, 2>& value) const {
  return base::StrCat(
      {SVGNumberToString(value[0]), u" ", SVGNumberToString(value[1])});
}

SVGPointList::SVGPointList(SVGAnimatedPointList& owner, bool read_only)
    : SVGTearOffList<SVGAnimatedPointList, SVGPoint, std::array<float, 2>>(
          owner,
          read_only) {}

SVGPointList::~SVGPointList() = default;

const bindings::WrapperTypeInfo* SVGPointList::GetWrapperTypeInfo() const {
  return &bindings::V8SVGPointList::kWrapperTypeInfo;
}

}  // namespace mitto
