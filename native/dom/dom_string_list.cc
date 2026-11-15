#include "mitto/dom/dom_string_list.h"

#include <algorithm>
#include <utility>

#include "mitto/bindings/v8_dom_string_list.h"

namespace mitto {

DOMStringList::DOMStringList(std::vector<std::u16string> strings)
    : strings_(std::move(strings)) {}

DOMStringList::~DOMStringList() = default;

// static
bindings::ScriptWrappable* DOMStringList::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<DOMStringList>(
      info, bindings::V8DOMStringList::kWrapperTypeInfo);
}

const bindings::WrapperTypeInfo* DOMStringList::GetWrapperTypeInfo() const {
  return &bindings::V8DOMStringList::kWrapperTypeInfo;
}

std::optional<std::u16string> DOMStringList::Item(uint32_t index) const {
  if (index >= strings_.size()) {
    return std::nullopt;
  }
  return strings_[index];
}

bool DOMStringList::Contains(const std::u16string& string) const {
  // Blink's contains() is a linear search, as the lists are short.
  return std::find(strings_.begin(), strings_.end(), string) != strings_.end();
}

void DOMStringList::Sort() {
  // CodeUnitCompareLessThan, which std::u16string's own comparison is.
  std::sort(strings_.begin(), strings_.end());
}

}  // namespace mitto
