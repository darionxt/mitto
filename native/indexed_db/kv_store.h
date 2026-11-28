// An ordered store of bytes, which IndexedDB's backing store of one storage
// key keeps all of its databases in, as Chrome's on-disk store keeps them in
// one LevelDB: keys sort as memcmp() sorts them, and a batch of writes goes
// in whole or not at all. The memory store holds it until the Session goes,
// and in a Session with a storage directory it is also what the file holds of
// the storage key, which each commit writes through to.
//
// Upstream: content/browser/indexed_db/instance/leveldb/backing_store.cc

#ifndef MITTO_INDEXED_DB_KV_STORE_H_
#define MITTO_INDEXED_DB_KV_STORE_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"

namespace mitto {

// A batch of writes: each key's new value, or nullopt to remove it.
using KvBatch = std::map<std::string, std::optional<std::string>>;

// The entries of a store, in the order of their keys.
using KvEntries = std::map<std::string, std::string, std::less<>>;

class KvStore {
 public:
  virtual ~KvStore() = default;

  virtual std::optional<std::string> Get(std::string_view key) = 0;
  virtual bool Has(std::string_view key) const = 0;
  // The first entry at or after `key` (or at or before it, `reverse`), within
  // [`lower`, `upper`), or nullopt. Its value lives until
  // what gave it changes.
  struct Entry {
    std::string key;
    std::string_view value;
  };
  virtual std::optional<Entry> Seek(std::string_view key,
                                    std::string_view lower,
                                    std::string_view upper,
                                    bool reverse) = 0;
  // Writes all of `batch`, or nothing, and returns whether it did.
  virtual bool Commit(const KvBatch& batch) = 0;
  // How many bytes of keys and values it holds.
  virtual int64_t Size() const = 0;
};

class MemoryKvStore final : public KvStore {
 public:
  // What takes each batch once it is committed, to keep it elsewhere.
  using Writer = base::RepeatingCallback<void(const KvBatch& batch)>;

  MemoryKvStore();
  // A store that holds `entries` to start with, and hands each batch that it
  // commits to `writer`.
  MemoryKvStore(KvEntries entries, Writer writer);
  MemoryKvStore(const MemoryKvStore&) = delete;
  MemoryKvStore& operator=(const MemoryKvStore&) = delete;
  ~MemoryKvStore() override;

  std::optional<std::string> Get(std::string_view key) override;
  bool Has(std::string_view key) const override;
  std::optional<Entry> Seek(std::string_view key,
                            std::string_view lower,
                            std::string_view upper,
                            bool reverse) override;
  bool Commit(const KvBatch& batch) override;
  int64_t Size() const override;

 private:
  KvEntries entries_;
  int64_t size_ = 0;
  const Writer writer_;
};

}  // namespace mitto

#endif  // MITTO_INDEXED_DB_KV_STORE_H_
