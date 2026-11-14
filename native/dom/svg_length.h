// SVG's lengths: SVGLength, whose value is a CSS value as Blink holds it, a
// number in a unit, a math function or text with var(), and SVGAnimatedLength,
// an SVG element's lengths of an attribute, whose baseVal reads the attribute
// as Blink parses it and writes back what a page sets, and whose animVal is
// the same, but read-only, as no animation runs.
//
// A length in an absolute unit converts as Blink converts it. A relative one
// needs its element in a document, or it throws Blink's NotSupportedError.
// Then a percentage resolves against the nearest svg element's viewBox, or
// its width and height, and the window for an outermost one, and the other
// relative units resolve as a media query's do, against the initial font and
// the window, where Blink reads the element's style and layout. A math
// function holds no percentage yet, as the math functions of native/css
// take none, so one with a percentage does not parse.
//
// Upstream: third_party/blink/renderer/core/svg/svg_length.cc
// Upstream: third_party/blink/renderer/core/svg/svg_length_tear_off.cc
// Upstream: third_party/blink/renderer/core/svg/svg_length_context.cc
// Upstream: third_party/blink/renderer/core/svg/svg_animated_length.cc
// Upstream: third_party/blink/renderer/core/svg/svg_text_content_element.cc

#ifndef MITTO_DOM_SVG_LENGTH_H_
#define MITTO_DOM_SVG_LENGTH_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>

#include "mitto/bindings/script_wrappable.h"
#include "mitto/css/css_math_expression.h"
#include "mitto/css/css_token.h"
#include "mitto/dom/svg_animated_values.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class SVGAnimatedLength;
class SVGElement;

// What an SVG length holds, as Blink's SVGLength holds a CSS value.
class SVGLengthValue {
 public:
  // A number in `unit`, kNumber for a user unit.
  static SVGLengthValue Literal(double value, CssUnit unit);
  // A number of no unit that Blink's SVGAnimatedTextLength gives, whose unit
  // type is SVG_LENGTHTYPE_UNKNOWN.
  static SVGLengthValue UnknownNumber(double value);
  // Blink's SetValueAsString(): the attribute as the CSS parser reads the
  // property x in SVG's attribute mode, or nothing.
  static std::optional<SVGLengthValue> Parse(std::u16string_view text);

  SVGLengthValue(const SVGLengthValue&);
  SVGLengthValue& operator=(const SVGLengthValue&);
  ~SVGLengthValue();

  bool IsNumeric() const { return kind_ == Kind::kLiteral; }
  bool IsUnparsed() const { return kind_ == Kind::kUnparsed; }
  // A literal's number and unit.
  double Number() const { return value_; }
  CssUnit Unit() const { return unit_; }
  // The interface's unit type: SVG_LENGTHTYPE_UNKNOWN for a unit that it has
  // no constant for, a math function or var().
  uint16_t UnitType() const;
  // Blink's IsRelative(): a percentage, or a literal of a relative unit.
  bool IsRelative() const;
  std::u16string CssText() const;
  // A math function's value in user units, or 0 for anything else.
  double ComputeMath(const CssLengthResolver& resolver) const;

 private:
  enum class Kind { kLiteral, kMath, kUnparsed };

  explicit SVGLengthValue(Kind kind);

  Kind kind_;
  double value_ = 0;
  CssUnit unit_ = CssUnit::kNumber;
  bool unknown_number_ = false;
  std::optional<CssMathFunction> math_;
  std::u16string unparsed_;
};

class SVGLength final : public bindings::ScriptWrappable {
 public:
  // createSVGLength()'s: a user unit of 0, of no element.
  static SVGLength* Create();
  // An animated length's baseVal or, read-only, its animVal.
  static SVGLength* CreateTearOff(SVGAnimatedLength& owner, bool is_anim_val);
  // An object of a list's item, read-only for an animVal's, bound to the list
  // that gave it, if any.
  static SVGLength* ForItem(SVGListItem<SVGLengthValue>& item,
                            bool read_only,
                            SVGAnimatedListBase* binding);
  // The one a Known Gap gives, such as SVGLengthList's getItem() until lists
  // come: a length of no element, whose wrapper is of `info`.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  SVGLength(SVGAnimatedLength* owner, bool is_anim_val);
  SVGLength(SVGListItem<SVGLengthValue>& item,
            bool read_only,
            SVGAnimatedListBase* binding);
  ~SVGLength() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  uint16_t UnitType() const;
  float Value(bindings::ExceptionState& exception_state) const;
  void SetValue(float value, bindings::ExceptionState& exception_state);
  float ValueInSpecifiedUnits() const;
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

  // Blink's GetValueForInsertionFromTearOff(): the item that `list` takes,
  // this length's own, which binds it to `list`, when it is bound to no
  // element, or else a copy. A length that a list gave stays bound to it,
  // even once a change to the attribute takes its item out.
  SVGListItem<SVGLengthValue>* ItemToInsert(SVGAnimatedListBase& list);

 private:
  SVGLengthValue Target() const;
  // Blink's CommitChange(): the animated length's base value, which writes
  // its attribute, or its item's, which its list writes.
  void Commit(const SVGLengthValue& value);
  SVGElement* ContextElement() const;
  SVGLengthMode Mode() const;
  // Throws Blink's NoModificationAllowedError for an animVal.
  bool ThrowIfImmutable(bindings::ExceptionState& exception_state) const;

  const cppgc::Member<SVGAnimatedLength> owner_;
  const bool is_anim_val_;
  // The item of a list's length, or a length of its own, without an owner.
  const cppgc::Member<SVGListItem<SVGLengthValue>> item_;
  // The list that an item's length is bound to: its element is the length's.
  cppgc::Member<SVGAnimatedListBase> binding_;
};

class SVGAnimatedLength final : public SVGAnimatedPropertyBase {
 public:
  // Blink's SVGLength::Initial.
  enum class Initial {
    kUnitlessZero,
    kPercentMinus10,
    kPercent0,
    kPercent50,
    kPercent100,
    kPercent120,
    kNumber3,
  };

  // `text_length` makes textLength's, whose base value with no attribute is
  // the text's computed length, which a Page with no layout has as 0.
  static SVGAnimatedLength* Create(SVGElement& element,
                                   std::u16string_view attribute,
                                   SVGLengthMode mode,
                                   Initial initial,
                                   bool text_length = false);

  SVGAnimatedLength(SVGElement& element,
                    std::u16string_view attribute,
                    SVGLengthMode mode,
                    Initial initial,
                    bool text_length);
  ~SVGAnimatedLength() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface: the same objects on every read.
  SVGLength* BaseVal();
  SVGLength* AnimVal();

  const SVGLengthValue& BaseValue();
  // Sets the base value and writes it to the attribute.
  void SetBaseValue(const SVGLengthValue& value);
  SVGElement& Element() const { return ContextElement(); }
  SVGLengthMode Mode() const { return mode_; }

 protected:
  void Parse(const std::u16string* value) override;

 private:
  const SVGLengthMode mode_;
  const Initial initial_;
  const bool text_length_;
  SVGLengthValue value_;
  cppgc::Member<SVGLength> base_val_;
  cppgc::Member<SVGLength> anim_val_;
};

}  // namespace mitto

#endif  // MITTO_DOM_SVG_LENGTH_H_
