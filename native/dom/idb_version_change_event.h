// IndexedDB's IDBVersionChangeEvent: the old and new versions of a database,
// which upgradeneeded, blocked, versionchange and a delete's success carry.
// It never bubbles and cannot be canceled, even when a page's init dictionary
// says so, as Blink makes it.
//
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_version_change_event.h
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_version_change_event.cc

#ifndef MITTO_DOM_IDB_VERSION_CHANGE_EVENT_H_
#define MITTO_DOM_IDB_VERSION_CHANGE_EVENT_H_

#include <stdint.h>

#include <optional>
#include <string>

#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/event.h"

namespace mitto {

class IDBVersionChangeEvent final : public Event {
 public:
  // `new IDBVersionChangeEvent(type, init)`.
  static IDBVersionChangeEvent* Create(
      const std::u16string& type,
      const bindings::IDBVersionChangeEventInit& init);
  // A trusted event, as IndexedDB fires one, with the data loss of an
  // upgrade.
  static IDBVersionChangeEvent* CreateTrusted(
      const std::u16string& type,
      uint64_t old_version,
      std::optional<uint64_t> new_version,
      bool data_loss = false,
      const std::u16string& data_loss_message = std::u16string());

  IDBVersionChangeEvent(const std::u16string& type,
                        uint64_t old_version,
                        std::optional<uint64_t> new_version,
                        bool data_loss,
                        const std::u16string& data_loss_message);
  ~IDBVersionChangeEvent() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  uint64_t OldVersion() const { return old_version_; }
  std::optional<uint64_t> NewVersion() const { return new_version_; }
  // "none" or "total".
  std::u16string DataLoss() const { return data_loss_ ? u"total" : u"none"; }
  std::u16string DataLossMessage() const { return data_loss_message_; }

 private:
  const uint64_t old_version_;
  const std::optional<uint64_t> new_version_;
  // blink.mojom.IDBDataLoss::Total.
  const bool data_loss_;
  const std::u16string data_loss_message_;
};

}  // namespace mitto

#endif  // MITTO_DOM_IDB_VERSION_CHANGE_EVENT_H_
