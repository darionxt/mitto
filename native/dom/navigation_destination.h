// The HTML Standard's NavigationDestination, a navigate event's destination:
// where a navigation goes, the entry of a traversal, which #280's are, and the
// state that it carries, which getState() gives a copy of.
//
// Upstream: third_party/blink/renderer/core/navigation_api/navigation_destination.h

#ifndef MITTO_DOM_NAVIGATION_DESTINATION_H_
#define MITTO_DOM_NAVIGATION_DESTINATION_H_

#include <stdint.h>

#include <optional>
#include <string>

#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/serialized_script_value.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-value.h"

namespace mitto {

class NavigationHistoryEntry;

class NavigationDestination final : public bindings::ScriptWrappable {
 public:
  static NavigationDestination* Create(
      std::u16string url,
      bool same_document,
      std::optional<SerializedScriptValue> state,
      NavigationHistoryEntry* entry = nullptr);

  NavigationDestination(std::u16string url,
                        bool same_document,
                        std::optional<SerializedScriptValue> state,
                        NavigationHistoryEntry* entry);
  ~NavigationDestination() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface: the entry's key, ID and index for a
  // traversal, and "", "" and -1 otherwise.
  std::u16string Key() const;
  std::u16string Id() const;
  const std::u16string& Url() const { return url_; }
  int64_t Index() const;
  bool SameDocument() const { return same_document_; }
  v8::Local<v8::Value> GetState() const;

 private:
  const std::u16string url_;
  const bool same_document_;
  const std::optional<SerializedScriptValue> state_;
  cppgc::Member<NavigationHistoryEntry> entry_;
};

}  // namespace mitto

#endif  // MITTO_DOM_NAVIGATION_DESTINATION_H_
