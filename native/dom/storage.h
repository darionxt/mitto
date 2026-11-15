// HTML's Storage, which localStorage and sessionStorage give: Blink's
// StorageArea, one for each window and kind, over the Page's cached area of
// the window's storage key, which the Session keeps. Its named properties are
// the area's keys.
//
// Upstream: third_party/blink/renderer/modules/storage/storage_area.cc

#ifndef MITTO_DOM_STORAGE_H_
#define MITTO_DOM_STORAGE_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class CachedStorageArea;
class Window;

class Storage final : public bindings::ScriptWrappable {
 public:
  // The messages of the SecurityError that a window without access to its
  // storage gets.
  static constexpr char kAccessDataMessage[] =
      "Storage is disabled inside 'data:' URLs.";
  static constexpr char kAccessDeniedMessage[] =
      "Access is denied for this document.";
  static constexpr char kAccessSandboxedMessage[] =
      "The document is sandboxed and lacks the 'allow-same-origin' flag.";

  static Storage* Create(Window& window,
                         scoped_refptr<CachedStorageArea> cached_area);

  Storage(Window& window, scoped_refptr<CachedStorageArea> cached_area);
  ~Storage() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  uint32_t Length(bindings::ExceptionState& exception_state) const;
  std::optional<std::u16string> Key(
      uint32_t index,
      bindings::ExceptionState& exception_state) const;
  std::optional<std::u16string> GetItem(
      const std::u16string& key,
      bindings::ExceptionState& exception_state) const;
  void SetItem(const std::u16string& key,
               const std::u16string& value,
               bindings::ExceptionState& exception_state);
  // Whether the named property deleter deleted the property, which it always
  // does unless it throws, as Blink's kDeleted.
  bool RemoveItem(const std::u16string& key,
                  bindings::ExceptionState& exception_state);
  void Clear(bindings::ExceptionState& exception_state);

  // The named properties: the keys, in the order of key().
  std::vector<std::u16string> NamedPropertyEnumerator() const;
  bool NamedPropertyQuery(const std::u16string& name) const;

  // What the cached area asks of the window: its document's URL, in UTF-8,
  // and to queue a storage event at it. Returns false once the window has
  // gone, so that the area stops asking.
  std::string PageUrl() const;
  bool EnqueueStorageEvent(const std::optional<std::u16string>& key,
                           const std::optional<std::u16string>& old_value,
                           const std::optional<std::u16string>& new_value,
                           const std::string& url);

 private:
  // Whether the window can still reach its storage: not once it has gone.
  bool CanAccessStorage() const;

  const cppgc::WeakMember<Window> window_;
  const scoped_refptr<CachedStorageArea> cached_area_;
};

}  // namespace mitto

#endif  // MITTO_DOM_STORAGE_H_
