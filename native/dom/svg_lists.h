// SVG's lists of numbers, lengths and strings: SVGNumber, SVGNumberList and
// SVGAnimatedNumberList, SVGLengthList and SVGAnimatedLengthList, and
// SVGStringList, as Blink's list tear-offs give them.
//
// An animated list's items are the attribute's, as Blink parses it, and its
// objects share each item: a change through one writes the attribute again,
// with no record for mutation observers. getItem() gives a new object each
// time. Inserting an object of no element, such as createSVGNumber()'s, puts
// its own item in the list, so a change through it reaches the list too, and
// inserting any other puts a copy in, as an object that a list gave stays
// bound to it. A change to the attribute
// replaces the items, and the objects of the old ones keep their values. An
// animVal's list and items are read-only. An empty list removes its
// attribute, as Blink's null value of an empty list does.
//
// Upstream: third_party/blink/renderer/core/svg/properties/svg_list_property_tear_off_helper.h
// Upstream: third_party/blink/renderer/core/svg/properties/svg_list_property.cc
// Upstream: third_party/blink/renderer/core/svg/svg_number_list.cc
// Upstream: third_party/blink/renderer/core/svg/svg_length_list.cc
// Upstream: third_party/blink/renderer/core/svg/svg_string_list.cc
// Upstream: third_party/blink/renderer/core/svg/svg_string_list_tear_off.h

#ifndef MITTO_DOM_SVG_LISTS_H_
#define MITTO_DOM_SVG_LISTS_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/heap_vector.h"
#include "mitto/dom/svg_animated_values.h"
#include "mitto/dom/svg_length.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class SVGElement;

// An animated list's items, which the attribute gives.
template <typename Value>
class SVGAnimatedItemList : public SVGAnimatedListBase {
 public:
  void Trace(cppgc::Visitor* visitor) const override {
    SVGAnimatedListBase::Trace(visitor);
    items_.Trace(visitor);
  }

  const HeapVector<SVGListItem<Value>>& Items() {
    EnsureFresh();
    return items_;
  }
  // Each takes or gives the list's items, and writes the attribute.
  void ClearItems();
  void InsertItem(size_t index, SVGListItem<Value>& item);
  SVGListItem<Value>* RemoveItem(size_t index);
  void ReplaceItem(size_t index, SVGListItem<Value>& item);
  // Blink's initialize(): `item` alone, which writes the attribute once.
  void ResetItems(SVGListItem<Value>& item);

 protected:
  SVGAnimatedItemList(SVGElement& element, std::u16string_view attribute)
      : SVGAnimatedListBase(element, attribute) {}

  // The attribute's values, or nothing for one that does not parse, which
  // leaves the list empty.
  virtual std::optional<std::vector<Value>> ParseValues(
      std::u16string_view text) const = 0;
  virtual std::u16string ValueText(const Value& value) const = 0;

  // SVGAnimatedPropertyBase:
  void Parse(const std::u16string* value) override;
  // SVGAnimatedListBase:
  bool Holds(const void* item) const override;
  void Synchronize() override;

 private:
  HeapVector<SVGListItem<Value>> items_;
};

// The interface of a list of objects of class `TearOff`: SVGNumberList's and
// SVGLengthList's members.
template <typename Owner, typename TearOff, typename Value>
class SVGTearOffList : public bindings::ScriptWrappable {
 public:
  void Trace(cppgc::Visitor* visitor) const override {
    bindings::ScriptWrappable::Trace(visitor);
    visitor->Trace(owner_);
  }

  // The members of the interface.
  uint32_t Length() const;
  void Clear(bindings::ExceptionState& exception_state);
  TearOff* Initialize(TearOff* new_item,
                      bindings::ExceptionState& exception_state);
  TearOff* GetItem(uint32_t index, bindings::ExceptionState& exception_state);
  TearOff* InsertItemBefore(TearOff* new_item,
                            uint32_t index,
                            bindings::ExceptionState& exception_state);
  TearOff* ReplaceItem(TearOff* new_item,
                       uint32_t index,
                       bindings::ExceptionState& exception_state);
  TearOff* RemoveItem(uint32_t index,
                      bindings::ExceptionState& exception_state);
  TearOff* AppendItem(TearOff* new_item,
                      bindings::ExceptionState& exception_state);
  void AnonymousIndexedSetter(uint32_t index,
                              TearOff* new_item,
                              bindings::ExceptionState& exception_state);

 protected:
  SVGTearOffList(Owner& owner, bool read_only)
      : owner_(&owner), read_only_(read_only) {}

 private:
  bool ThrowIfReadOnly(bindings::ExceptionState& exception_state) const;
  bool ThrowIfOutOfRange(uint32_t index,
                         bindings::ExceptionState& exception_state) const;

  const cppgc::Member<Owner> owner_;
  const bool read_only_;
};

class SVGNumber final : public bindings::ScriptWrappable {
 public:
  // createSVGNumber()'s: 0, in no list.
  static SVGNumber* Create();
  static SVGNumber* ForItem(SVGListItem<float>& item,
                            bool read_only,
                            SVGAnimatedListBase* binding);

  SVGNumber(SVGListItem<float>& item,
            bool read_only,
            SVGAnimatedListBase* binding);
  ~SVGNumber() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  float Value() const { return item_->value; }
  void SetValue(float value, bindings::ExceptionState& exception_state);

  // Blink's GetValueForInsertionFromTearOff(): what `list` takes, this
  // number's item, which binds it to `list`, when it is bound to no list, or
  // else a copy.
  SVGListItem<float>* ItemToInsert(SVGAnimatedListBase& list);

 private:
  const cppgc::Member<SVGListItem<float>> item_;
  const bool read_only_;
  // The list that gave it or took it, which it stays bound to.
  cppgc::Member<SVGAnimatedListBase> binding_;
};

class SVGNumberList;

class SVGAnimatedNumberList final : public SVGAnimatedItemList<float> {
 public:
  static SVGAnimatedNumberList* Create(SVGElement& element,
                                       std::u16string_view attribute);

  SVGAnimatedNumberList(SVGElement& element, std::u16string_view attribute);
  ~SVGAnimatedNumberList() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface: the same objects on every read.
  SVGNumberList* BaseVal();
  SVGNumberList* AnimVal();

 protected:
  std::optional<std::vector<float>> ParseValues(
      std::u16string_view text) const override;
  std::u16string ValueText(const float& value) const override;

 private:
  cppgc::Member<SVGNumberList> base_val_;
  cppgc::Member<SVGNumberList> anim_val_;
};

class SVGNumberList final
    : public SVGTearOffList<SVGAnimatedNumberList, SVGNumber, float> {
 public:
  SVGNumberList(SVGAnimatedNumberList& owner, bool read_only);
  ~SVGNumberList() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

class SVGLengthList;

class SVGAnimatedLengthList final : public SVGAnimatedItemList<SVGLengthValue> {
 public:
  static SVGAnimatedLengthList* Create(SVGElement& element,
                                       std::u16string_view attribute,
                                       SVGLengthMode mode);

  SVGAnimatedLengthList(SVGElement& element,
                        std::u16string_view attribute,
                        SVGLengthMode mode);
  ~SVGAnimatedLengthList() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface: the same objects on every read.
  SVGLengthList* BaseVal();
  SVGLengthList* AnimVal();

  // SVGAnimatedListBase:
  SVGLengthMode LengthMode() const override;

 protected:
  std::optional<std::vector<SVGLengthValue>> ParseValues(
      std::u16string_view text) const override;
  std::u16string ValueText(const SVGLengthValue& value) const override;

 private:
  const SVGLengthMode mode_;
  cppgc::Member<SVGLengthList> base_val_;
  cppgc::Member<SVGLengthList> anim_val_;
};

class SVGLengthList final
    : public SVGTearOffList<SVGAnimatedLengthList, SVGLength, SVGLengthValue> {
 public:
  SVGLengthList(SVGAnimatedLengthList& owner, bool read_only);
  ~SVGLengthList() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
};

// SVGTests' requiredExtensions, whose strings spaces part, and
// systemLanguage, whose strings commas part. Its strings are the attribute's,
// which each change writes again without reading it back, so a string with
// the delimiter in it stays one.
class SVGStringList final : public SVGAnimatedPropertyBase {
 public:
  static SVGStringList* Create(SVGElement& element,
                               std::u16string_view attribute,
                               char16_t delimiter);

  SVGStringList(SVGElement& element,
                std::u16string_view attribute,
                char16_t delimiter);
  ~SVGStringList() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  uint32_t Length();
  void Clear(bindings::ExceptionState& exception_state);
  std::u16string Initialize(const std::u16string& new_item,
                            bindings::ExceptionState& exception_state);
  std::u16string GetItem(uint32_t index,
                         bindings::ExceptionState& exception_state);
  std::u16string InsertItemBefore(const std::u16string& new_item,
                                  uint32_t index,
                                  bindings::ExceptionState& exception_state);
  std::u16string ReplaceItem(const std::u16string& new_item,
                             uint32_t index,
                             bindings::ExceptionState& exception_state);
  std::u16string RemoveItem(uint32_t index,
                            bindings::ExceptionState& exception_state);
  std::u16string AppendItem(const std::u16string& new_item,
                            bindings::ExceptionState& exception_state);
  void AnonymousIndexedSetter(uint32_t index,
                              const std::u16string& new_item,
                              bindings::ExceptionState& exception_state);

 protected:
  void Parse(const std::u16string* value) override;

 private:
  const std::vector<std::u16string>& Values();
  // Sets the strings and writes the attribute.
  void Write(std::vector<std::u16string> values);

  const char16_t delimiter_;
  std::vector<std::u16string> values_;
};

}  // namespace mitto

#endif  // MITTO_DOM_SVG_LISTS_H_
