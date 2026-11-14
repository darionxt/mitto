// The HTML Standard's NavigationHistoryEntry: an entry of a tab's session
// history, which navigation.currentEntry and entries() give, with keys and IDs
// that are random UUIDs, as Blink's are, the state that the Navigation API
// gave it, and the document it belongs to, by its document sequence number,
// which sameDocument compares. Once its window has gone, its key and ID are
// empty and its index is -1.
//
// Upstream: third_party/blink/renderer/core/navigation_api/navigation_history_entry.cc

#ifndef MITTO_DOM_NAVIGATION_HISTORY_ENTRY_H_
#define MITTO_DOM_NAVIGATION_HISTORY_ENTRY_H_

#include <stdint.h>

#include <optional>
#include <string>

#include "mitto/dom/event_target.h"
#include "mitto/dom/serialized_script_value.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-value.h"

namespace mitto {

class Window;

class NavigationHistoryEntry final : public EventTarget {
 public:
  // An entry of `window`'s session history at `url`, of the document of
  // `document_sequence`.
  static NavigationHistoryEntry* Create(
      Window* window,
      std::u16string key,
      std::u16string id,
      std::u16string url,
      uint64_t document_sequence,
      std::optional<SerializedScriptValue> state);
  // A new random key or ID, as Blink makes one for a HistoryItem.
  static std::u16string NewKey();
  // One of no window, with no keys and no URL, for a Known Gap that gives
  // one.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  NavigationHistoryEntry(Window* window,
                         std::u16string key,
                         std::u16string id,
                         std::u16string url,
                         uint64_t document_sequence,
                         std::optional<SerializedScriptValue> state);
  ~NavigationHistoryEntry() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  std::u16string Key() const;
  std::u16string Id() const;
  std::optional<std::u16string> Url() const;
  int64_t Index() const;
  bool SameDocument() const;
  // A copy of its state, undefined for none.
  v8::Local<v8::Value> GetState() const;

  // What the window's Navigation uses.
  const std::u16string& key() const { return key_; }
  const std::u16string& id() const { return id_; }
  const std::u16string& url() const { return url_; }
  uint64_t document_sequence() const { return document_sequence_; }
  const std::optional<SerializedScriptValue>& state() const { return state_; }
  void SetState(std::optional<SerializedScriptValue> state) {
    state_ = std::move(state);
  }

 private:
  // Whether its window still has its context.
  bool Attached() const;

  cppgc::Member<Window> window_;
  const std::u16string key_;
  const std::u16string id_;
  const std::u16string url_;
  const uint64_t document_sequence_;
  std::optional<SerializedScriptValue> state_;
};

}  // namespace mitto

#endif  // MITTO_DOM_NAVIGATION_HISTORY_ENTRY_H_
