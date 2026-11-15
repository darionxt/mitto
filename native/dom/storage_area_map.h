// A Page's copy of a storage area, as Blink's StorageAreaMap keeps it: the keys
// and values, the quota that a write may not take the area past, and the order
// of key(), which is the order of Blink's hash table, not of insertion. That
// order comes from WTF's HashMap<String, String>: open addressing with a
// triangular probe from Blink's hash of the key, a table that starts at 8
// buckets, doubles when half full and halves when a sixth full, and buckets
// that a removal leaves deleted until the next rehash.
//
// Upstream: third_party/blink/renderer/modules/storage/storage_area_map.cc
// Upstream: third_party/blink/renderer/platform/wtf/hash_table.h

#ifndef MITTO_DOM_STORAGE_AREA_MAP_H_
#define MITTO_DOM_STORAGE_AREA_MAP_H_

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

namespace mitto {

class StorageAreaMap {
 public:
  // `quota` in bytes, two for each character of keys and values.
  explicit StorageAreaMap(size_t quota);

  StorageAreaMap(const StorageAreaMap&) = delete;
  StorageAreaMap& operator=(const StorageAreaMap&) = delete;

  ~StorageAreaMap();

  unsigned GetLength() const { return key_count_; }
  // The key at `index`, in the hash table's order, if there is one.
  std::optional<std::u16string> GetKey(unsigned index) const;
  std::optional<std::u16string> GetItem(const std::u16string& key) const;

  // Returns false if the quota would be exceeded. A write that does not grow
  // the area is never refused. `old_value`, if not null, gets the value before.
  bool SetItem(const std::u16string& key,
               const std::u16string& value,
               std::optional<std::u16string>* old_value);
  void SetItemIgnoringQuota(const std::u16string& key,
                            const std::u16string& value);
  // Returns false if there was no such item.
  bool RemoveItem(const std::u16string& key,
                  std::optional<std::u16string>* old_value);

  size_t quota_used() const { return quota_used_; }

 private:
  struct Bucket {
    enum class State : uint8_t { kEmpty, kDeleted, kFull };
    State state = State::kEmpty;
    std::u16string key;
    std::u16string value;
  };

  bool SetItemInternal(const std::u16string& key,
                       const std::u16string& value,
                       std::optional<std::u16string>* old_value,
                       bool check_quota);

  // The bucket that holds `key`, or none.
  Bucket* Find(const std::u16string& key);
  const Bucket* Find(const std::u16string& key) const;
  // Adds `key` with `value`, which the table does not hold.
  void Insert(std::u16string key, std::u16string value);
  void Erase(Bucket& bucket);
  void Expand();
  void Rehash(size_t new_table_size);
  // The first empty bucket on `key`'s probe, in a table with no deleted ones.
  Bucket& BucketToReinsert(std::vector<Bucket>& table,
                           const std::u16string& key);

  void ResetKeyIterator() const;

  std::vector<Bucket> table_;
  size_t key_count_ = 0;
  size_t deleted_count_ = 0;

  // GetKey() walks from the last index it gave, as Blink's map keeps an
  // iterator, so that a loop over key() is not quadratic.
  mutable unsigned last_key_index_ = 0;
  mutable size_t last_key_bucket_ = 0;

  size_t quota_used_ = 0;
  const size_t quota_;
};

}  // namespace mitto

#endif  // MITTO_DOM_STORAGE_AREA_MAP_H_
