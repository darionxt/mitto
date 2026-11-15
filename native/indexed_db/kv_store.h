// An ordered store of bytes, which IndexedDB's backing store of one storage
// key keeps all of its databases in, as Chrome's on-disk store keeps them in
// one LevelDB: keys sort as memcmp() sorts them, and a batch of writes goes
// in whole or not at all. The memory store holds it until the Session goes.
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

namespace mitto {

// A batch of writes: each key's new value, or nullopt to remove it.
using KvBatch = std::map<std::string, std::optional<std::string>>;

class KvStore {
 public:
  virtual ~KvStore() = default;

  virtual std::optional<std::string> Get(std::string_view key) = 0;
  // The first entry at or after `key` (or at or before it, `reverse`), within
  // [`lower`, `upper`), or nullopt.
  struct Entry {
    std::string key;
    std::string value;
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
  MemoryKvStore();
  MemoryKvStore(const MemoryKvStore&) = delete;
  MemoryKvStore& operator=(const MemoryKvStore&) = delete;
  ~MemoryKvStore() override;

  std::optional<std::string> Get(std::string_view key) override;
  std::optional<Entry> Seek(std::string_view key,
                            std::string_view lower,
                            std::string_view upper,
                            bool reverse) override;
  bool Commit(const KvBatch& batch) override;
  int64_t Size() const override;

 private:
  std::map<std::string, std::string, std::less<>> entries_;
  int64_t size_ = 0;
};

}  // namespace mitto

#endif  // MITTO_INDEXED_DB_KV_STORE_H_
