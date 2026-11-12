// The HTML Standard's NavigationHistoryEntry: an entry of a tab's session
// history, which navigation.currentEntry gives for the document, with keys of
// its own, random as Blink's are. getState() is a Known Gap, whose undefined
// is what an entry without state gives.
//
// Upstream: third_party/blink/renderer/core/navigation_api/navigation_history_entry.cc

#ifndef MITTO_DOM_NAVIGATION_HISTORY_ENTRY_H_
#define MITTO_DOM_NAVIGATION_HISTORY_ENTRY_H_

#include <stdint.h>

#include <optional>
#include <string>

#include "mitto/dom/event_target.h"

namespace mitto {

class NavigationHistoryEntry final : public EventTarget {
 public:
  // The entry of a document at `url`, the first of its tab's history.
  static NavigationHistoryEntry* Create(std::u16string url);
  // One of no document, with no keys and no URL, for a Known Gap that gives
  // one.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  NavigationHistoryEntry(std::u16string key,
                         std::u16string id,
                         std::optional<std::u16string> url,
                         int64_t index,
                         bool same_document);
  ~NavigationHistoryEntry() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  const std::u16string& Key() const { return key_; }
  const std::u16string& Id() const { return id_; }
  const std::optional<std::u16string>& Url() const { return url_; }
  int64_t Index() const { return index_; }
  bool SameDocument() const { return same_document_; }

 private:
  const std::u16string key_;
  const std::u16string id_;
  const std::optional<std::u16string> url_;
  const int64_t index_;
  const bool same_document_;
};

}  // namespace mitto

#endif  // MITTO_DOM_NAVIGATION_HISTORY_ENTRY_H_
