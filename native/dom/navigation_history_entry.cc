#include "mitto/dom/navigation_history_entry.h"

#include <utility>

#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "mitto/bindings/v8_navigation_history_entry.h"

namespace mitto {

// static
NavigationHistoryEntry* NavigationHistoryEntry::Create(std::u16string url) {
  // Blink's keys and IDs are random UUIDs, made for the entry.
  return bindings::MakeGarbageCollected<NavigationHistoryEntry>(
      base::UTF8ToUTF16(base::Uuid::GenerateRandomV4().AsLowercaseString()),
      base::UTF8ToUTF16(base::Uuid::GenerateRandomV4().AsLowercaseString()),
      std::move(url), 0, true);
}

// static
bindings::ScriptWrappable* NavigationHistoryEntry::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<NavigationHistoryEntry>(
      info, bindings::V8NavigationHistoryEntry::kWrapperTypeInfo,
      std::u16string(), std::u16string(), std::nullopt, 0, false);
}

NavigationHistoryEntry::NavigationHistoryEntry(
    std::u16string key,
    std::u16string id,
    std::optional<std::u16string> url,
    int64_t index,
    bool same_document)
    : key_(std::move(key)),
      id_(std::move(id)),
      url_(std::move(url)),
      index_(index),
      same_document_(same_document) {}

NavigationHistoryEntry::~NavigationHistoryEntry() = default;

const bindings::WrapperTypeInfo* NavigationHistoryEntry::GetWrapperTypeInfo()
    const {
  return &bindings::V8NavigationHistoryEntry::kWrapperTypeInfo;
}

}  // namespace mitto
