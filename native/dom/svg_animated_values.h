// The SVG DOM's animated strings, booleans, enumerations, integers and
// numbers: an SVG element's object for an attribute, whose baseVal reads the
// attribute as Blink parses it and keeps what a page sets, writing it back as
// Blink writes it, and whose animVal is the same, as no animation runs. A
// value that does not parse is the attribute's initial value, as is a missing
// one. Writing the attribute back makes no record for mutation observers, as
// Blink writes it lazily, when something reads it.
//
// Upstream: third_party/blink/renderer/core/svg/properties/svg_animated_property.h
// Upstream: third_party/blink/renderer/core/svg/svg_animated_href.cc
// Upstream: third_party/blink/renderer/core/svg/svg_animated_enumeration_base.cc
// Upstream: third_party/blink/renderer/core/svg/svg_number.cc
// Upstream: third_party/blink/renderer/core/svg/svg_number_optional_number.cc
// Upstream: third_party/blink/renderer/core/svg/svg_integer.cc
// Upstream: third_party/blink/renderer/core/svg/svg_integer_optional_integer.cc
// Upstream: third_party/blink/renderer/core/svg/svg_boolean.cc
// Upstream: third_party/blink/renderer/core/svg/svg_enumeration.cc
// Upstream: third_party/blink/renderer/core/svg/svg_parser_utilities.cc

#ifndef MITTO_DOM_SVG_ANIMATED_VALUES_H_
#define MITTO_DOM_SVG_ANIMATED_VALUES_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>

#include "base/containers/span.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/memory/raw_ref.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/qualified_name.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class SVGElement;

// The keywords of an enumeration, the first of which is its value 1, and the
// largest value that a page can see and set. Blink has values past that one
// for keywords that came later, such as feComposite's lighter, which read as
// 0.
struct SVGEnumerationMap {
  // RAW_PTR_EXCLUSION: #global-scope, as the maps are constant tables.
  RAW_PTR_EXCLUSION base::span<const std::u16string_view> keywords;
  uint16_t max_exposed_value;
};

// Which number of an attribute of two, such as stdDeviation's, a value is.
enum class SVGPairHalf { kNone, kFirst, kSecond };

// What a page cannot see of the values: the attribute that each reads, and
// whether the attribute changed since it was read.
class SVGAnimatedPropertyBase : public bindings::ScriptWrappable {
 public:
  ~SVGAnimatedPropertyBase() override;

  void Trace(cppgc::Visitor* visitor) const override;

  // The content attribute's local name, of no namespace.
  std::u16string_view AttributeName() const { return attribute_; }
  SVGPairHalf Half() const { return half_; }
  // Whether a change of the attribute `name` changes this value.
  virtual bool ReadsAttribute(const QualifiedName& name) const;
  // The attribute changed by other means than this value's setter, so the
  // value is read from it again.
  void AttributeChanged() { stale_ = true; }

 protected:
  SVGAnimatedPropertyBase(SVGElement& element,
                          std::u16string_view attribute,
                          SVGPairHalf half);

  SVGElement& ContextElement() const { return *element_; }
  const std::u16string* AttributeValue() const;
  // Blink's SynchronizeAttribute(): the attribute takes `value`, with no
  // record for mutation observers and without being parsed again.
  void SynchronizeAttribute(const std::u16string& value);
  // Reads the attribute again if it changed since it was last read.
  void EnsureFresh();
  virtual void Parse(const std::u16string* value) = 0;

 private:
  const cppgc::Member<SVGElement> element_;
  // A name of static storage, from the element's tables.
  const std::u16string_view attribute_;
  const SVGPairHalf half_;
  bool stale_ = true;
};

class SVGAnimatedString final : public SVGAnimatedPropertyBase {
 public:
  // `attribute` "href" reads href, or xlink:href while only that is set, as
  // Blink's SVGAnimatedHref does.
  static SVGAnimatedString* Create(SVGElement& element,
                                   std::u16string_view attribute);

  SVGAnimatedString(SVGElement& element, std::u16string_view attribute);
  ~SVGAnimatedString() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  std::u16string BaseVal() const;
  void SetBaseVal(const std::u16string& value,
                  bindings::ExceptionState& exception_state);
  std::u16string AnimVal() const { return BaseVal(); }

  // SVGAnimatedPropertyBase:
  bool ReadsAttribute(const QualifiedName& name) const override;

 protected:
  void Parse(const std::u16string* value) override {}

 private:
  // The attribute that the value is in: href's xlink:href while only that is
  // set.
  QualifiedName BackingName() const;
};

class SVGAnimatedBoolean final : public SVGAnimatedPropertyBase {
 public:
  static SVGAnimatedBoolean* Create(SVGElement& element,
                                    std::u16string_view attribute);

  SVGAnimatedBoolean(SVGElement& element, std::u16string_view attribute);
  ~SVGAnimatedBoolean() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  bool BaseVal();
  void SetBaseVal(bool value, bindings::ExceptionState& exception_state);
  bool AnimVal() { return BaseVal(); }

 protected:
  void Parse(const std::u16string* value) override;

 private:
  bool value_ = false;
};

class SVGAnimatedEnumeration final : public SVGAnimatedPropertyBase {
 public:
  static SVGAnimatedEnumeration* Create(SVGElement& element,
                                        std::u16string_view attribute,
                                        const SVGEnumerationMap& map,
                                        uint16_t initial_value);

  SVGAnimatedEnumeration(SVGElement& element,
                         std::u16string_view attribute,
                         const SVGEnumerationMap& map,
                         uint16_t initial_value);
  ~SVGAnimatedEnumeration() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  uint16_t BaseVal();
  void SetBaseVal(uint16_t value, bindings::ExceptionState& exception_state);
  uint16_t AnimVal() { return BaseVal(); }

 protected:
  void Parse(const std::u16string* value) override;

 private:
  const raw_ref<const SVGEnumerationMap> map_;
  const uint16_t initial_value_;
  uint16_t value_ = 0;
};

class SVGAnimatedInteger final : public SVGAnimatedPropertyBase {
 public:
  static SVGAnimatedInteger* Create(SVGElement& element,
                                    std::u16string_view attribute,
                                    int32_t initial_value);
  // The two of feConvolveMatrix's order, each positive or else both the
  // initial value, as Blink's SVGAnimatedOrder checks them.
  static SVGAnimatedInteger* CreateOrderHalf(SVGElement& element,
                                             SVGPairHalf half,
                                             int32_t initial_value);

  SVGAnimatedInteger(SVGElement& element,
                     std::u16string_view attribute,
                     SVGPairHalf half,
                     int32_t initial_value);
  ~SVGAnimatedInteger() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  int32_t BaseVal();
  void SetBaseVal(int32_t value, bindings::ExceptionState& exception_state);
  int32_t AnimVal() { return BaseVal(); }

  void SetOtherHalf(SVGAnimatedInteger& other) { other_half_ = &other; }

 protected:
  void Parse(const std::u16string* value) override;

 private:
  const int32_t initial_value_;
  int32_t value_ = 0;
  cppgc::Member<SVGAnimatedInteger> other_half_;
};

class SVGAnimatedNumber final : public SVGAnimatedPropertyBase {
 public:
  enum class Syntax {
    kNumber,
    // A number or a percentage, which is a hundredth of it, as a stop's
    // offset takes it.
    kNumberOrPercentage,
    // One number, or two that a space or comma parts, as stdDeviation takes
    // them: one gives both halves.
    kNumberOptionalNumber,
  };

  static SVGAnimatedNumber* Create(SVGElement& element,
                                   std::u16string_view attribute,
                                   float initial_value,
                                   Syntax syntax = Syntax::kNumber);
  static SVGAnimatedNumber* CreateHalf(SVGElement& element,
                                       std::u16string_view attribute,
                                       SVGPairHalf half,
                                       float initial_value);

  SVGAnimatedNumber(SVGElement& element,
                    std::u16string_view attribute,
                    SVGPairHalf half,
                    float initial_value,
                    Syntax syntax);
  ~SVGAnimatedNumber() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  float BaseVal();
  void SetBaseVal(float value, bindings::ExceptionState& exception_state);
  float AnimVal() { return BaseVal(); }

  void SetOtherHalf(SVGAnimatedNumber& other) { other_half_ = &other; }
  // The base value as the setter sets it, for the elements' own methods,
  // such as setStdDeviation().
  void SetBaseValue(float value);

 protected:
  void Parse(const std::u16string* value) override;

 private:
  const float initial_value_;
  const Syntax syntax_;
  float value_ = 0;
  cppgc::Member<SVGAnimatedNumber> other_half_;
};

// Blink's SVG number grammar, of svg_parser_utilities.cc: the whole of
// `value` with spaces around it, or nothing.
std::optional<float> ParseSVGNumber(std::u16string_view value);
// String::Number() of a float: six significant digits, as JavaScript's
// toPrecision(6) gives them, without the trailing zeros of a number with no
// exponent.
std::u16string SVGNumberToString(float value);

}  // namespace mitto

#endif  // MITTO_DOM_SVG_ANIMATED_VALUES_H_
