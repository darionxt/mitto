#include "mitto/dom/string_hash_table_order.h"

#include <utility>

#include "base/check.h"
#include "mitto/string_hasher.h"

namespace mitto {

namespace {

// HashTraits' kMinimumTableSize, the size of a table's first buckets.
constexpr size_t kMinimumTableSize = 8;
// HashTable's kMaxLoad: a table grows once it is half full.
constexpr size_t kMaxLoad = 2;

// HashTable::CalculateCapacity(): the smallest power of two above `size`,
// doubled.
size_t CalculateCapacity(size_t size) {
  for (size_t mask = size; mask; mask >>= 1) {
    size |= mask;
  }
  return (size + 1) * 2;
}

}  // namespace

StringHashTableOrder::StringHashTableOrder() = default;

// static
StringHashTableOrder StringHashTableOrder::WithCapacityFor(size_t size) {
  StringHashTableOrder table;
  size_t capacity = CalculateCapacity(size);
  if (capacity < kMinimumTableSize) {
    capacity = kMinimumTableSize;
  }
  table.buckets_.resize(capacity);
  return table;
}

StringHashTableOrder::StringHashTableOrder(const StringHashTableOrder&) =
    default;
StringHashTableOrder& StringHashTableOrder::operator=(
    const StringHashTableOrder&) = default;
StringHashTableOrder::~StringHashTableOrder() = default;

void StringHashTableOrder::Insert(std::string_view key) {
  DCHECK(!key.empty());
  if (buckets_.empty()) {
    buckets_.resize(kMinimumTableSize);
  }
  Place(std::string(key));
  ++count_;
  // ShouldExpand(), then Expand(): a table with no deleted bucket doubles.
  if (count_ * kMaxLoad >= buckets_.size()) {
    Rehash(buckets_.size() * 2);
  }
}

std::vector<std::string> StringHashTableOrder::Keys() const {
  std::vector<std::string> keys;
  keys.reserve(count_);
  for (const std::string& key : buckets_) {
    if (!key.empty()) {
      keys.push_back(key);
    }
  }
  return keys;
}

void StringHashTableOrder::Place(std::string key) {
  const size_t mask = buckets_.size() - 1;
  size_t index = HashString24(key) & mask;
  for (size_t probe = 1; !buckets_[index].empty(); ++probe) {
    index = (index + probe) & mask;
  }
  buckets_[index] = std::move(key);
}

void StringHashTableOrder::Rehash(size_t size) {
  std::vector<std::string> old = std::move(buckets_);
  buckets_ = std::vector<std::string>(size);
  for (std::string& key : old) {
    if (!key.empty()) {
      Place(std::move(key));
    }
  }
}

}  // namespace mitto
