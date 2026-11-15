// A Page's side of one storage area, as Blink's CachedStorageArea is: the whole
// map of the area, loaded on first use with one call that waits for the
// Session, each change sent to the Session on its own, and the changes that the
// Session tells of applied, for every window of the Page that has the area's
// Storage object. A change that a window makes waits for the Session's echo,
// and meanwhile the key ignores changes from elsewhere, as Blink's pending
// mutations do. sessionStorage's changes have no echo: the Page is the source
// of truth for its own sessionStorage, as Blink's renderer is.
//
// Upstream: third_party/blink/renderer/modules/storage/cached_storage_area.cc

#ifndef MITTO_DOM_CACHED_STORAGE_AREA_H_
#define MITTO_DOM_CACHED_STORAGE_AREA_H_

#include <stdint.h>

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ref.h"
#include "base/memory/ref_counted.h"
#include "base/token.h"
#include "mitto/dom/storage_area_map.h"
#include "mitto/page_messages.h"
#include "v8/include/cppgc/persistent.h"

namespace mitto {

class Storage;
class StorageBackend;

// The Page's StorageController keeps one for each kind and storage key, and
// each window's Storage object holds the one it uses, off V8's heap, as Blink
// keeps it.
class CachedStorageArea final : public base::RefCounted<CachedStorageArea> {
 public:
  // Binds the area of `kind` of the storage key of `document`, the ID of the
  // document whose window first reaches it, through `backend`, which outlives
  // it. Other windows of the same storage key share it.
  CachedStorageArea(PageStorageKind kind,
                    uint64_t document,
                    StorageBackend& backend);

  CachedStorageArea(const CachedStorageArea&) = delete;
  CachedStorageArea& operator=(const CachedStorageArea&) = delete;

  // What Storage's members do. `source` is the Storage object of the window
  // that asks, which RegisterSource() registered.
  unsigned GetLength();
  std::optional<std::u16string> GetKey(unsigned index);
  std::optional<std::u16string> GetItem(const std::u16string& key);
  // Returns false if the change would take the area past its quota.
  bool SetItem(const std::u16string& key,
               const std::u16string& value,
               Storage* source);
  void RemoveItem(const std::u16string& key, Storage* source);
  void Clear(Storage* source);

  // A window's Storage object, which hears of the changes that others make,
  // until it goes.
  void RegisterSource(Storage* source);

  // What the Session tells the area's observer `observer` of, on the main
  // thread: StorageAreaObserver. What comes for an observer that another
  // has replaced is dropped.
  void OnEvent(PageMessage type, const PageStorageEvent& event);

  // The bytes that its map holds, as Blink counts its cache.
  size_t memory_used() const { return map_ ? map_->quota_used() : 0; }

 private:
  friend class base::RefCounted<CachedStorageArea>;

  // Lets go of the area in the Session.
  ~CachedStorageArea();

  // A change that a window made, until the Session's echo of it comes.
  struct PendingMutation {
    std::optional<std::u16string> key;
    std::optional<std::u16string> new_value;
    std::optional<std::u16string> old_value;
  };

  bool IsSessionStorage() const { return kind_ == PageStorageKind::kSession; }

  void KeyChanged(const PageStorageEvent& event);
  void KeyChangeFailed(const PageStorageEvent& event);
  void KeyDeleted(const PageStorageEvent& event);
  void AllDeleted(const PageStorageEvent& event);

  void EnqueuePendingMutation(std::optional<std::u16string> key,
                              std::optional<std::u16string> new_value,
                              std::optional<std::u16string> old_value,
                              const base::Token& source_id);
  std::unique_ptr<PendingMutation> PopPendingMutation(
      const base::Token& source_id);
  void MaybeApplyNonLocalMutationForKey(
      const std::u16string& key,
      const std::optional<std::u16string>& new_value);

  // Loads the map if it has not loaded, waiting for the Session.
  void EnsureLoaded();

  // Each registered window but the source's hears of a change that changed
  // something.
  void EnqueueStorageEvent(const std::optional<std::u16string>& key,
                           const std::optional<std::u16string>& old_value,
                           const std::optional<std::u16string>& new_value,
                           const std::string& url,
                           const base::Token& source_id);

  PageStorageSource SourceOf(Storage* source) const;

  // Between the strings of the Storage interface and what goes to the
  // Session, in the formats of Blink's FormatOption.
  std::string KeyToBytes(const std::u16string& key) const;
  std::string ValueToBytes(const std::u16string& value) const;
  std::u16string BytesToKey(const std::string& bytes) const;
  std::u16string BytesToValue(const std::string& bytes) const;

  const PageStorageKind kind_;
  const raw_ref<StorageBackend> backend_;
  // The area's number in the Session, and its observer's, or 0 once unbound.
  uint64_t area_ = 0;
  uint64_t observer_ = 0;

  std::unique_ptr<StorageAreaMap> map_;

  // The changes of each window that wait for their echo, in order, and the
  // same by key.
  std::map<base::Token, std::deque<std::unique_ptr<PendingMutation>>>
      pending_mutations_by_source_;
  std::map<std::u16string, std::deque<PendingMutation*>>
      pending_mutations_by_key_;

  // The windows' Storage objects, with the token of each.
  std::vector<std::pair<cppgc::WeakPersistent<Storage>, base::Token>> sources_;
};

}  // namespace mitto

#endif  // MITTO_DOM_CACHED_STORAGE_AREA_H_
