// The table follows third_party/blink/renderer/platform/wtf/hash_table.h, under
// the license in that directory.

#include "mitto/http_header_map.h"

#include <stddef.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mitto/string_hasher.h"

namespace mitto {
namespace {

// HashTable::kMaxLoad and HashTraits::kMinimumTableSize.
constexpr size_t kMaxLoad = 2;
constexpr size_t kMinimumTableSize = 8;

using Buckets = std::vector<std::optional<std::string_view>>;

void Insert(Buckets& buckets, std::string_view name) {
  const size_t mask = buckets.size() - 1;
  size_t index = CaseFoldingHashString24(name) & mask;
  size_t probe_count = 0;
  while (buckets[index]) {
    ++probe_count;
    index = (index + probe_count) & mask;
  }
  buckets[index] = name;
}

// HashTable::Rehash(), which goes through the old table in its own order.
Buckets Rehash(const Buckets& old_buckets, size_t new_size) {
  Buckets buckets(new_size);
  for (const std::optional<std::string_view>& name : old_buckets) {
    if (name) {
      Insert(buckets, *name);
    }
  }
  return buckets;
}

}  // namespace

std::vector<std::string> HTTPHeaderMapOrder(
    base::span<const std::string_view> names) {
  Buckets buckets(kMinimumTableSize);
  size_t key_count = 0;
  for (std::string_view name : names) {
    Insert(buckets, name);
    ++key_count;
    if (key_count * kMaxLoad >= buckets.size()) {
      buckets = Rehash(buckets, buckets.size() * 2);
    }
  }

  std::vector<std::string> order;
  order.reserve(names.size());
  for (const std::optional<std::string_view>& name : buckets) {
    if (name) {
      order.emplace_back(*name);
    }
  }
  return order;
}

}  // namespace mitto
