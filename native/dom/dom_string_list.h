// The HTML Standard's DOMStringList, a list of strings that a page reads by
// index: IndexedDB's lists of object store and index names, and a top-level
// window's location.ancestorOrigins, which is empty.
//
// Upstream: third_party/blink/renderer/core/dom/dom_string_list.h
// Upstream: third_party/blink/renderer/core/dom/dom_string_list.cc

#ifndef MITTO_DOM_DOM_STRING_LIST_H_
#define MITTO_DOM_DOM_STRING_LIST_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "mitto/bindings/script_wrappable.h"

namespace mitto {

class DOMStringList final : public bindings::ScriptWrappable {
 public:
  explicit DOMStringList(std::vector<std::u16string> strings = {});
  ~DOMStringList() override;

  // An empty list, for a Known Gap that gives one, as ancestorOrigins does.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  uint32_t Length() const { return static_cast<uint32_t>(strings_.size()); }
  // Null past the end.
  std::optional<std::u16string> Item(uint32_t index) const;
  bool Contains(const std::u16string& string) const;

  // What Blink's IndexedDB uses to fill one.
  void Append(const std::u16string& string) { strings_.push_back(string); }
  // Sorts the strings by their code units.
  void Sort();

 private:
  std::vector<std::u16string> strings_;
};

}  // namespace mitto

#endif  // MITTO_DOM_DOM_STRING_LIST_H_
