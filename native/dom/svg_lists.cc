#include "mitto/dom/svg_lists.h"

#include <algorithm>
#include <utility>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_svg_animated_length_list.h"
#include "mitto/bindings/v8_svg_animated_number_list.h"
#include "mitto/bindings/v8_svg_length_list.h"
#include "mitto/bindings/v8_svg_number.h"
#include "mitto/bindings/v8_svg_number_list.h"
#include "mitto/bindings/v8_svg_string_list.h"
#include "mitto/dom/svg_element.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

namespace {

bool IsHTMLSpace(char16_t c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

// ExceptionMessages::IndexExceedsMaximumBound().
std::string IndexExceedsMaximumBound(uint32_t given, size_t bound) {
  return base::StrCat(
      {"The index provided (", base::NumberToString(given),
       ") is greater than ", given == bound ? "or equal to " : "",
       "the maximum bound (", base::NumberToString(bound), ")."});
}

void ThrowReadOnly(bindings::ExceptionState& exception_state) {
  exception_state.ThrowDOMException("NoModificationAllowedError",
                                    "This object is read-only.");
}

}  // namespace

// SVGAnimatedItemList.

template <typename Value>
void SVGAnimatedItemList<Value>::Parse(const std::u16string* value) {
  for (size_t i = 0; i < items_.size(); ++i) {
    items_[i]->owner = nullptr;
  }
  items_.EraseIf([](SVGListItem<Value>*) { return true; });
  if (!value) {
    return;
  }
  std::optional<std::vector<Value>> values = ParseValues(*value);
  if (!values) {
    return;
  }
  for (Value& item_value : *values) {
    SVGListItem<Value>* const item =
        bindings::MakeGarbageCollected<SVGListItem<Value>>(
            std::move(item_value));
    item->owner = this;
    items_.push_back(item);
  }
}

template <typename Value>
bool SVGAnimatedItemList<Value>::Holds(const void* item) const {
  for (size_t i = 0; i < items_.size(); ++i) {
    if (items_[i] == item) {
      return true;
    }
  }
  return false;
}

template <typename Value>
void SVGAnimatedItemList<Value>::Synchronize() {
  if (items_.empty()) {
    SynchronizeAttribute(std::nullopt);
    return;
  }
  std::u16string text;
  for (size_t i = 0; i < items_.size(); ++i) {
    if (i) {
      text += u' ';
    }
    text += ValueText(items_[i]->value);
  }
  SynchronizeAttribute(text);
}

template <typename Value>
void SVGAnimatedItemList<Value>::ClearItems() {
  EnsureFresh();
  for (size_t i = 0; i < items_.size(); ++i) {
    items_[i]->owner = nullptr;
  }
  items_.EraseIf([](SVGListItem<Value>*) { return true; });
  Synchronize();
}

template <typename Value>
void SVGAnimatedItemList<Value>::InsertItem(size_t index,
                                            SVGListItem<Value>& item) {
  EnsureFresh();
  item.owner = this;
  items_.insert(std::min(index, items_.size()), &item);
  Synchronize();
}

template <typename Value>
SVGListItem<Value>* SVGAnimatedItemList<Value>::RemoveItem(size_t index) {
  EnsureFresh();
  SVGListItem<Value>* const item = items_[index];
  items_.EraseIf([item](SVGListItem<Value>* each) { return each == item; });
  item->owner = nullptr;
  Synchronize();
  return item;
}

template <typename Value>
void SVGAnimatedItemList<Value>::ReplaceItem(size_t index,
                                             SVGListItem<Value>& item) {
  EnsureFresh();
  SVGListItem<Value>* const old_item = items_[index];
  items_.EraseIf(
      [old_item](SVGListItem<Value>* each) { return each == old_item; });
  old_item->owner = nullptr;
  item.owner = this;
  items_.insert(index, &item);
  Synchronize();
}

template <typename Value>
void SVGAnimatedItemList<Value>::ResetItems(SVGListItem<Value>& item) {
  EnsureFresh();
  for (size_t i = 0; i < items_.size(); ++i) {
    items_[i]->owner = nullptr;
  }
  items_.EraseIf([](SVGListItem<Value>*) { return true; });
  item.owner = this;
  items_.push_back(&item);
  Synchronize();
}

// SVGTearOffList.

template <typename Owner, typename TearOff, typename Value>
bool SVGTearOffList<Owner, TearOff, Value>::ThrowIfReadOnly(
    bindings::ExceptionState& exception_state) const {
  if (read_only_) {
    ThrowReadOnly(exception_state);
  }
  return read_only_;
}

template <typename Owner, typename TearOff, typename Value>
bool SVGTearOffList<Owner, TearOff, Value>::ThrowIfOutOfRange(
    uint32_t index,
    bindings::ExceptionState& exception_state) const {
  const size_t length = owner_->Items().size();
  if (index < length) {
    return false;
  }
  exception_state.ThrowDOMException("IndexSizeError",
                                    IndexExceedsMaximumBound(index, length));
  return true;
}

template <typename Owner, typename TearOff, typename Value>
uint32_t SVGTearOffList<Owner, TearOff, Value>::Length() const {
  return static_cast<uint32_t>(owner_->Items().size());
}

template <typename Owner, typename TearOff, typename Value>
void SVGTearOffList<Owner, TearOff, Value>::Clear(
    bindings::ExceptionState& exception_state) {
  if (ThrowIfReadOnly(exception_state)) {
    return;
  }
  owner_->ClearItems();
}

template <typename Owner, typename TearOff, typename Value>
TearOff* SVGTearOffList<Owner, TearOff, Value>::Initialize(
    TearOff* new_item,
    bindings::ExceptionState& exception_state) {
  if (ThrowIfReadOnly(exception_state)) {
    return nullptr;
  }
  SVGListItem<Value>* const item = new_item->ItemToInsert();
  owner_->ResetItems(*item);
  return TearOff::ForItem(*item, /*read_only=*/false);
}

template <typename Owner, typename TearOff, typename Value>
TearOff* SVGTearOffList<Owner, TearOff, Value>::GetItem(
    uint32_t index,
    bindings::ExceptionState& exception_state) {
  if (ThrowIfOutOfRange(index, exception_state)) {
    return nullptr;
  }
  return TearOff::ForItem(*owner_->Items()[index], read_only_);
}

template <typename Owner, typename TearOff, typename Value>
TearOff* SVGTearOffList<Owner, TearOff, Value>::InsertItemBefore(
    TearOff* new_item,
    uint32_t index,
    bindings::ExceptionState& exception_state) {
  if (ThrowIfReadOnly(exception_state)) {
    return nullptr;
  }
  SVGListItem<Value>* const item = new_item->ItemToInsert();
  owner_->InsertItem(index, *item);
  return TearOff::ForItem(*item, /*read_only=*/false);
}

template <typename Owner, typename TearOff, typename Value>
TearOff* SVGTearOffList<Owner, TearOff, Value>::ReplaceItem(
    TearOff* new_item,
    uint32_t index,
    bindings::ExceptionState& exception_state) {
  if (ThrowIfReadOnly(exception_state) ||
      ThrowIfOutOfRange(index, exception_state)) {
    return nullptr;
  }
  SVGListItem<Value>* const item = new_item->ItemToInsert();
  owner_->ReplaceItem(index, *item);
  return TearOff::ForItem(*item, /*read_only=*/false);
}

template <typename Owner, typename TearOff, typename Value>
TearOff* SVGTearOffList<Owner, TearOff, Value>::RemoveItem(
    uint32_t index,
    bindings::ExceptionState& exception_state) {
  if (ThrowIfReadOnly(exception_state) ||
      ThrowIfOutOfRange(index, exception_state)) {
    return nullptr;
  }
  return TearOff::ForItem(*owner_->RemoveItem(index), /*read_only=*/false);
}

template <typename Owner, typename TearOff, typename Value>
TearOff* SVGTearOffList<Owner, TearOff, Value>::AppendItem(
    TearOff* new_item,
    bindings::ExceptionState& exception_state) {
  if (ThrowIfReadOnly(exception_state)) {
    return nullptr;
  }
  SVGListItem<Value>* const item = new_item->ItemToInsert();
  owner_->InsertItem(owner_->Items().size(), *item);
  return TearOff::ForItem(*item, /*read_only=*/false);
}

template <typename Owner, typename TearOff, typename Value>
void SVGTearOffList<Owner, TearOff, Value>::AnonymousIndexedSetter(
    uint32_t index,
    TearOff* new_item,
    bindings::ExceptionState& exception_state) {
  ReplaceItem(new_item, index, exception_state);
}

template class SVGAnimatedItemList<float>;
template class SVGAnimatedItemList<SVGLengthValue>;
template class SVGTearOffList<SVGAnimatedNumberList, SVGNumber, float>;
template class SVGTearOffList<SVGAnimatedLengthList, SVGLength, SVGLengthValue>;

// SVGNumber.

// static
SVGNumber* SVGNumber::Create() {
  return bindings::MakeGarbageCollected<SVGNumber>(
      *bindings::MakeGarbageCollected<SVGListItem<float>>(0.0f), false);
}

// static
SVGNumber* SVGNumber::ForItem(SVGListItem<float>& item, bool read_only) {
  return bindings::MakeGarbageCollected<SVGNumber>(item, read_only);
}

SVGNumber::SVGNumber(SVGListItem<float>& item, bool read_only)
    : item_(&item), read_only_(read_only) {}

SVGNumber::~SVGNumber() = default;

const bindings::WrapperTypeInfo* SVGNumber::GetWrapperTypeInfo() const {
  return &bindings::V8SVGNumber::kWrapperTypeInfo;
}

void SVGNumber::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(item_);
}

void SVGNumber::SetValue(float value,
                         bindings::ExceptionState& exception_state) {
  if (read_only_) {
    ThrowReadOnly(exception_state);
    return;
  }
  item_->value = value;
  if (item_->owner) {
    item_->owner->ItemChanged(item_.Get());
  }
}

SVGListItem<float>* SVGNumber::ItemToInsert() const {
  if (!item_->owner && !read_only_) {
    return item_.Get();
  }
  return bindings::MakeGarbageCollected<SVGListItem<float>>(item_->value);
}

// SVGAnimatedNumberList.

// static
SVGAnimatedNumberList* SVGAnimatedNumberList::Create(
    SVGElement& element,
    std::u16string_view attribute) {
  return bindings::MakeGarbageCollected<SVGAnimatedNumberList>(element,
                                                               attribute);
}

SVGAnimatedNumberList::SVGAnimatedNumberList(SVGElement& element,
                                             std::u16string_view attribute)
    : SVGAnimatedItemList<float>(element, attribute) {}

SVGAnimatedNumberList::~SVGAnimatedNumberList() = default;

const bindings::WrapperTypeInfo* SVGAnimatedNumberList::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGAnimatedNumberList::kWrapperTypeInfo;
}

void SVGAnimatedNumberList::Trace(cppgc::Visitor* visitor) const {
  SVGAnimatedItemList<float>::Trace(visitor);
  visitor->Trace(base_val_);
  visitor->Trace(anim_val_);
}

SVGNumberList* SVGAnimatedNumberList::BaseVal() {
  if (!base_val_) {
    base_val_ = bindings::MakeGarbageCollected<SVGNumberList>(*this, false);
  }
  return base_val_.Get();
}

SVGNumberList* SVGAnimatedNumberList::AnimVal() {
  if (!anim_val_) {
    anim_val_ = bindings::MakeGarbageCollected<SVGNumberList>(*this, true);
  }
  return anim_val_.Get();
}

// SVGNumberList::Parse(): numbers in Blink's grammar, which spaces or a comma
// part.
std::optional<std::vector<float>> SVGAnimatedNumberList::ParseValues(
    std::u16string_view text) const {
  return ParseSVGNumberList(text);
}

std::u16string SVGAnimatedNumberList::ValueText(const float& value) const {
  return SVGNumberToString(value);
}

SVGNumberList::SVGNumberList(SVGAnimatedNumberList& owner, bool read_only)
    : SVGTearOffList<SVGAnimatedNumberList, SVGNumber, float>(owner,
                                                              read_only) {}

SVGNumberList::~SVGNumberList() = default;

const bindings::WrapperTypeInfo* SVGNumberList::GetWrapperTypeInfo() const {
  return &bindings::V8SVGNumberList::kWrapperTypeInfo;
}

// SVGAnimatedLengthList.

// static
SVGAnimatedLengthList* SVGAnimatedLengthList::Create(
    SVGElement& element,
    std::u16string_view attribute,
    SVGLengthMode mode) {
  return bindings::MakeGarbageCollected<SVGAnimatedLengthList>(element,
                                                               attribute, mode);
}

SVGAnimatedLengthList::SVGAnimatedLengthList(SVGElement& element,
                                             std::u16string_view attribute,
                                             SVGLengthMode mode)
    : SVGAnimatedItemList<SVGLengthValue>(element, attribute), mode_(mode) {}

SVGAnimatedLengthList::~SVGAnimatedLengthList() = default;

const bindings::WrapperTypeInfo* SVGAnimatedLengthList::GetWrapperTypeInfo()
    const {
  return &bindings::V8SVGAnimatedLengthList::kWrapperTypeInfo;
}

void SVGAnimatedLengthList::Trace(cppgc::Visitor* visitor) const {
  SVGAnimatedItemList<SVGLengthValue>::Trace(visitor);
  visitor->Trace(base_val_);
  visitor->Trace(anim_val_);
}

SVGLengthMode SVGAnimatedLengthList::LengthMode() const {
  return mode_;
}

SVGLengthList* SVGAnimatedLengthList::BaseVal() {
  if (!base_val_) {
    base_val_ = bindings::MakeGarbageCollected<SVGLengthList>(*this, false);
  }
  return base_val_.Get();
}

SVGLengthList* SVGAnimatedLengthList::AnimVal() {
  if (!anim_val_) {
    anim_val_ = bindings::MakeGarbageCollected<SVGLengthList>(*this, true);
  }
  return anim_val_.Get();
}

// SVGLengthList::ParseInternal(): lengths that spaces or a comma part, each
// as an SVGLength parses it. An empty token ends the list, and one that does
// not parse empties it.
std::optional<std::vector<SVGLengthValue>> SVGAnimatedLengthList::ParseValues(
    std::u16string_view text) const {
  std::vector<SVGLengthValue> values;
  size_t position = 0;
  while (position < text.size()) {
    size_t end = position;
    while (end < text.size() && text[end] != u',' && !IsHTMLSpace(text[end])) {
      ++end;
    }
    if (end == position) {
      break;
    }
    std::optional<SVGLengthValue> value =
        SVGLengthValue::Parse(text.substr(position, end - position));
    if (!value) {
      return std::nullopt;
    }
    values.push_back(std::move(*value));
    position = end;
    // SkipOptionalSVGSpacesOrDelimiter().
    while (position < text.size() && IsHTMLSpace(text[position])) {
      ++position;
    }
    if (position < text.size() && text[position] == u',') {
      ++position;
      while (position < text.size() && IsHTMLSpace(text[position])) {
        ++position;
      }
    }
  }
  return values;
}

std::u16string SVGAnimatedLengthList::ValueText(
    const SVGLengthValue& value) const {
  return value.CssText();
}

SVGLengthList::SVGLengthList(SVGAnimatedLengthList& owner, bool read_only)
    : SVGTearOffList<SVGAnimatedLengthList, SVGLength, SVGLengthValue>(
          owner,
          read_only) {}

SVGLengthList::~SVGLengthList() = default;

const bindings::WrapperTypeInfo* SVGLengthList::GetWrapperTypeInfo() const {
  return &bindings::V8SVGLengthList::kWrapperTypeInfo;
}

// SVGStringList.

// static
SVGStringList* SVGStringList::Create(SVGElement& element,
                                     std::u16string_view attribute,
                                     char16_t delimiter) {
  return bindings::MakeGarbageCollected<SVGStringList>(element, attribute,
                                                       delimiter);
}

SVGStringList::SVGStringList(SVGElement& element,
                             std::u16string_view attribute,
                             char16_t delimiter)
    : SVGAnimatedPropertyBase(element, attribute, SVGPairHalf::kNone),
      delimiter_(delimiter) {}

SVGStringList::~SVGStringList() = default;

const bindings::WrapperTypeInfo* SVGStringList::GetWrapperTypeInfo() const {
  return &bindings::V8SVGStringList::kWrapperTypeInfo;
}

// ParseCommaSeparated() keeps each string between commas, without its spaces,
// and ParseSpaceSeparated() each run of what is not a space.
void SVGStringList::Parse(const std::u16string* value) {
  values_.clear();
  if (!value) {
    return;
  }
  const std::u16string& text = *value;
  if (delimiter_ == u',') {
    size_t start = 0;
    while (true) {
      const size_t comma = text.find(u',', start);
      std::u16string_view token = std::u16string_view(text).substr(
          start,
          comma == std::u16string::npos ? std::u16string::npos : comma - start);
      while (!token.empty() && IsHTMLSpace(token.front())) {
        token.remove_prefix(1);
      }
      while (!token.empty() && IsHTMLSpace(token.back())) {
        token.remove_suffix(1);
      }
      values_.emplace_back(token);
      if (comma == std::u16string::npos) {
        return;
      }
      start = comma + 1;
    }
  }
  size_t position = 0;
  while (true) {
    while (position < text.size() && IsHTMLSpace(text[position])) {
      ++position;
    }
    if (position == text.size()) {
      return;
    }
    size_t end = position;
    while (end < text.size() && !IsHTMLSpace(text[end])) {
      ++end;
    }
    values_.push_back(text.substr(position, end - position));
    position = end;
  }
}

const std::vector<std::u16string>& SVGStringList::Values() {
  EnsureFresh();
  return values_;
}

void SVGStringList::Write(std::vector<std::u16string> values) {
  EnsureFresh();
  values_ = std::move(values);
  if (values_.empty()) {
    SynchronizeAttribute(std::nullopt);
    return;
  }
  std::u16string text;
  for (size_t i = 0; i < values_.size(); ++i) {
    if (i) {
      text += delimiter_;
    }
    text += values_[i];
  }
  SynchronizeAttribute(text);
}

uint32_t SVGStringList::Length() {
  return static_cast<uint32_t>(Values().size());
}

void SVGStringList::Clear(bindings::ExceptionState& exception_state) {
  Write({});
}

std::u16string SVGStringList::Initialize(
    const std::u16string& new_item,
    bindings::ExceptionState& exception_state) {
  Write({new_item});
  return new_item;
}

std::u16string SVGStringList::GetItem(
    uint32_t index,
    bindings::ExceptionState& exception_state) {
  const std::vector<std::u16string> values = Values();
  if (index >= values.size()) {
    exception_state.ThrowDOMException(
        "IndexSizeError", IndexExceedsMaximumBound(index, values.size()));
    return std::u16string();
  }
  return values[index];
}

std::u16string SVGStringList::InsertItemBefore(
    const std::u16string& new_item,
    uint32_t index,
    bindings::ExceptionState& exception_state) {
  std::vector<std::u16string> values = Values();
  values.insert(values.begin() + std::min<size_t>(index, values.size()),
                new_item);
  Write(values);
  return new_item;
}

std::u16string SVGStringList::ReplaceItem(
    const std::u16string& new_item,
    uint32_t index,
    bindings::ExceptionState& exception_state) {
  std::vector<std::u16string> values = Values();
  if (index >= values.size()) {
    exception_state.ThrowDOMException(
        "IndexSizeError", IndexExceedsMaximumBound(index, values.size()));
    return std::u16string();
  }
  values[index] = new_item;
  Write(values);
  return new_item;
}

std::u16string SVGStringList::RemoveItem(
    uint32_t index,
    bindings::ExceptionState& exception_state) {
  std::vector<std::u16string> values = Values();
  if (index >= values.size()) {
    exception_state.ThrowDOMException(
        "IndexSizeError", IndexExceedsMaximumBound(index, values.size()));
    return std::u16string();
  }
  std::u16string removed = std::move(values[index]);
  values.erase(values.begin() + index);
  Write(values);
  return removed;
}

std::u16string SVGStringList::AppendItem(
    const std::u16string& new_item,
    bindings::ExceptionState& exception_state) {
  std::vector<std::u16string> values = Values();
  values.push_back(new_item);
  Write(values);
  return new_item;
}

void SVGStringList::AnonymousIndexedSetter(
    uint32_t index,
    const std::u16string& new_item,
    bindings::ExceptionState& exception_state) {
  ReplaceItem(new_item, index, exception_state);
}

}  // namespace mitto
