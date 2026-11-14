// HTML's StorageEvent, which a window gets when another window changes an area
// of its storage: Blink's StorageEvent. One that the Page fires neither bubbles
// nor can be canceled.
//
// Upstream: third_party/blink/renderer/modules/storage/storage_event.cc

#ifndef MITTO_DOM_STORAGE_EVENT_H_
#define MITTO_DOM_STORAGE_EVENT_H_

#include <optional>
#include <string>

#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/event.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

class Storage;

class StorageEvent final : public Event {
 public:
  // `new StorageEvent(type, init)`.
  static StorageEvent* Create(const std::u16string& type,
                              const bindings::StorageEventInit& init);
  // The event of a change to `storage_area`'s area, from the window of `url`:
  // a key without a value for one that went, and no key for a clear().
  static StorageEvent* CreateForChange(
      const std::optional<std::u16string>& key,
      const std::optional<std::u16string>& old_value,
      const std::optional<std::u16string>& new_value,
      const std::u16string& url,
      Storage* storage_area);

  StorageEvent(const std::u16string& type,
               const bindings::StorageEventInit& init);
  StorageEvent(const std::u16string& type,
               const bindings::EventInit& init,
               std::optional<std::u16string> key,
               std::optional<std::u16string> old_value,
               std::optional<std::u16string> new_value,
               std::u16string url,
               Storage* storage_area);
  ~StorageEvent() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  const std::optional<std::u16string>& Key() const { return key_; }
  const std::optional<std::u16string>& OldValue() const { return old_value_; }
  const std::optional<std::u16string>& NewValue() const { return new_value_; }
  const std::u16string& Url() const { return url_; }
  mitto::Storage* StorageArea() const { return storage_area_.Get(); }
  void InitStorageEvent(const std::u16string& type,
                        bool bubbles,
                        bool cancelable,
                        const std::optional<std::u16string>& key,
                        const std::optional<std::u16string>& old_value,
                        const std::optional<std::u16string>& new_value,
                        const std::u16string& url,
                        mitto::Storage* storage_area);

 private:
  std::optional<std::u16string> key_;
  std::optional<std::u16string> old_value_;
  std::optional<std::u16string> new_value_;
  std::u16string url_;
  cppgc::Member<mitto::Storage> storage_area_;
};

}  // namespace mitto

#endif  // MITTO_DOM_STORAGE_EVENT_H_
