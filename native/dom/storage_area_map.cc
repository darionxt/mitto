#include "mitto/dom/storage_area_map.h"

#include <algorithm>
#include <utility>

#include "base/check_op.h"
#include "base/containers/span.h"
#include "base/strings/string_view_util.h"
#include "mitto/string_hasher.h"

namespace mitto {

namespace {

// WTF's HashTable: the smallest table, and the loads at which it grows and
// shrinks.
constexpr size_t kMinimumTableSize = 8;
constexpr size_t kMaxLoad = 2;
constexpr size_t kMinLoad = 6;

// For quota purposes, each character counts as 2 bytes.
size_t QuotaForString(const std::u16string& s) {
  return s.size() * sizeof(char16_t);
}

// StringImpl::GetHash(): a string of Latin-1 characters hashes as its 8-bit
// form does, whichever form it has, and any other as the bytes of its UTF-16.
uint32_t HashOf(const std::u16string& s) {
  if (std::ranges::all_of(s, [](char16_t c) { return c <= 0xFF; })) {
    std::string latin1(s.size(), '\0');
    std::ranges::transform(s, latin1.begin(),
                           [](char16_t c) { return static_cast<char>(c); });
    return HashString24(latin1);
  }
  return HashString24(base::as_string_view(base::as_byte_span(s)));
}

}  // namespace

StorageAreaMap::StorageAreaMap(size_t quota) : quota_(quota) {}

StorageAreaMap::~StorageAreaMap() = default;

std::optional<std::u16string> StorageAreaMap::GetKey(unsigned index) const {
  if (index >= GetLength()) {
    return std::nullopt;
  }
  // From the start, or on from the last index given, whichever is nearer
  // without going back.
  if (index < last_key_index_ || last_key_index_ == 0) {
    last_key_index_ = 0;
    last_key_bucket_ = 0;
    while (table_[last_key_bucket_].state != Bucket::State::kFull) {
      ++last_key_bucket_;
    }
  }
  while (last_key_index_ < index) {
    ++last_key_bucket_;
    while (table_[last_key_bucket_].state != Bucket::State::kFull) {
      ++last_key_bucket_;
    }
    ++last_key_index_;
  }
  return table_[last_key_bucket_].key;
}

std::optional<std::u16string> StorageAreaMap::GetItem(
    const std::u16string& key) const {
  const Bucket* const bucket = Find(key);
  if (!bucket) {
    return std::nullopt;
  }
  return bucket->value;
}

bool StorageAreaMap::SetItem(const std::u16string& key,
                             const std::u16string& value,
                             std::optional<std::u16string>* old_value) {
  return SetItemInternal(key, value, old_value, /*check_quota=*/true);
}

void StorageAreaMap::SetItemIgnoringQuota(const std::u16string& key,
                                          const std::u16string& value) {
  SetItemInternal(key, value, nullptr, /*check_quota=*/false);
}

bool StorageAreaMap::RemoveItem(const std::u16string& key,
                                std::optional<std::u16string>* old_value) {
  Bucket* const bucket = Find(key);
  if (!bucket) {
    return false;
  }
  quota_used_ -= QuotaForString(key) + QuotaForString(bucket->value);
  if (old_value) {
    *old_value = std::move(bucket->value);
  }
  Erase(*bucket);
  ResetKeyIterator();
  return true;
}

bool StorageAreaMap::SetItemInternal(const std::u16string& key,
                                     const std::u16string& value,
                                     std::optional<std::u16string>* old_value,
                                     bool check_quota) {
  Bucket* const bucket = Find(key);
  size_t old_item_size = 0;
  if (bucket) {
    old_item_size = QuotaForString(key) + QuotaForString(bucket->value);
    if (old_value) {
      *old_value = bucket->value;
    }
  }
  DCHECK_GE(quota_used_, old_item_size);
  const size_t new_item_size = QuotaForString(key) + QuotaForString(value);
  const size_t new_quota_used = quota_used_ - old_item_size + new_item_size;
  // Only a write that grows the area is checked, so that an area over its
  // quota can still shrink.
  if (check_quota && new_item_size > old_item_size && new_quota_used > quota_) {
    return false;
  }
  // HashMap::Set(): a key that is there keeps its bucket.
  if (bucket) {
    bucket->value = value;
  } else {
    Insert(key, value);
  }
  ResetKeyIterator();
  quota_used_ = new_quota_used;
  return true;
}

StorageAreaMap::Bucket* StorageAreaMap::Find(const std::u16string& key) {
  return const_cast<Bucket*>(std::as_const(*this).Find(key));
}

const StorageAreaMap::Bucket* StorageAreaMap::Find(
    const std::u16string& key) const {
  if (table_.empty()) {
    return nullptr;
  }
  const size_t size_mask = table_.size() - 1;
  size_t i = HashOf(key) & size_mask;
  size_t probe_count = 0;
  while (true) {
    const Bucket& bucket = table_[i];
    if (bucket.state == Bucket::State::kEmpty) {
      return nullptr;
    }
    if (bucket.state == Bucket::State::kFull && bucket.key == key) {
      return &bucket;
    }
    ++probe_count;
    i = (i + probe_count) & size_mask;
  }
}

void StorageAreaMap::Insert(std::u16string key, std::u16string value) {
  if (table_.empty()) {
    Expand();
  }
  // HashTable::insert(): the probe goes on to an empty bucket, and the last
  // deleted bucket on the way takes the key, if there was one.
  const size_t size_mask = table_.size() - 1;
  size_t i = HashOf(key) & size_mask;
  size_t probe_count = 0;
  Bucket* deleted = nullptr;
  while (table_[i].state != Bucket::State::kEmpty) {
    if (table_[i].state == Bucket::State::kDeleted) {
      deleted = &table_[i];
    }
    ++probe_count;
    i = (i + probe_count) & size_mask;
  }
  Bucket& bucket = deleted ? *deleted : table_[i];
  if (deleted) {
    --deleted_count_;
  }
  bucket.state = Bucket::State::kFull;
  bucket.key = std::move(key);
  bucket.value = std::move(value);
  ++key_count_;
  if ((key_count_ + deleted_count_) * kMaxLoad >= table_.size()) {
    Expand();
  }
}

void StorageAreaMap::Erase(Bucket& bucket) {
  bucket.state = Bucket::State::kDeleted;
  bucket.key.clear();
  bucket.value.clear();
  ++deleted_count_;
  --key_count_;
  if (key_count_ * kMinLoad < table_.size() &&
      table_.size() > kMinimumTableSize) {
    Rehash(table_.size() / 2);
  }
}

void StorageAreaMap::Expand() {
  if (table_.empty()) {
    Rehash(kMinimumTableSize);
  } else if (key_count_ * kMinLoad < table_.size() * 2) {
    // Mostly deleted buckets: a rehash in place clears them.
    Rehash(table_.size());
  } else {
    Rehash(table_.size() * 2);
  }
}

void StorageAreaMap::Rehash(size_t new_table_size) {
  std::vector<Bucket> new_table(new_table_size);
  // In the order of the old table's buckets, each into the first empty bucket
  // on its probe.
  for (Bucket& bucket : table_) {
    if (bucket.state != Bucket::State::kFull) {
      continue;
    }
    Bucket& to = BucketToReinsert(new_table, bucket.key);
    to.state = Bucket::State::kFull;
    to.key = std::move(bucket.key);
    to.value = std::move(bucket.value);
  }
  table_ = std::move(new_table);
  deleted_count_ = 0;
}

StorageAreaMap::Bucket& StorageAreaMap::BucketToReinsert(
    std::vector<Bucket>& table,
    const std::u16string& key) {
  const size_t size_mask = table.size() - 1;
  size_t i = HashOf(key) & size_mask;
  size_t probe_count = 0;
  while (table[i].state != Bucket::State::kEmpty) {
    ++probe_count;
    i = (i + probe_count) & size_mask;
  }
  return table[i];
}

void StorageAreaMap::ResetKeyIterator() const {
  last_key_index_ = 0;
  last_key_bucket_ = 0;
}

}  // namespace mitto
