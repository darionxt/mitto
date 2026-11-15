#include "mitto/indexed_db/kv_store.h"

namespace mitto {

MemoryKvStore::MemoryKvStore() = default;
MemoryKvStore::~MemoryKvStore() = default;

std::optional<std::string> MemoryKvStore::Get(std::string_view key) {
  const auto found = entries_.find(key);
  if (found == entries_.end()) {
    return std::nullopt;
  }
  return found->second;
}

std::optional<KvStore::Entry> MemoryKvStore::Seek(std::string_view key,
                                                  std::string_view lower,
                                                  std::string_view upper,
                                                  bool reverse) {
  if (!reverse) {
    const auto found = entries_.lower_bound(key < lower ? lower : key);
    if (found == entries_.end() || found->first >= upper) {
      return std::nullopt;
    }
    return Entry{found->first, found->second};
  }
  // The last entry at or before `key`, below `upper`.
  auto found = entries_.upper_bound(key < upper ? key : upper);
  while (found != entries_.begin()) {
    --found;
    if (found->first < lower) {
      return std::nullopt;
    }
    if (found->first < upper && found->first <= key) {
      return Entry{found->first, found->second};
    }
  }
  return std::nullopt;
}

int64_t MemoryKvStore::Size() const {
  return size_;
}

bool MemoryKvStore::Commit(const KvBatch& batch) {
  for (const auto& [key, value] : batch) {
    const auto found = entries_.find(key);
    if (found != entries_.end()) {
      size_ -= static_cast<int64_t>(found->first.size() + found->second.size());
    }
    if (!value) {
      if (found != entries_.end()) {
        entries_.erase(found);
      }
      continue;
    }
    size_ += static_cast<int64_t>(key.size() + value->size());
    if (found != entries_.end()) {
      found->second = *value;
    } else {
      entries_.emplace(key, *value);
    }
  }
  return true;
}

}  // namespace mitto
