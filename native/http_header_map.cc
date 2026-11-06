// The hash below is rapidhash, Copyright 2024 Nicolas De Carli, under the BSD
// 2-Clause license in third_party/rapidhash/LICENSE of Chromium's source tree.
// The table follows third_party/blink/renderer/platform/wtf/hash_table.h, under
// the license in that directory.

#include "mitto/http_header_map.h"

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "base/check_op.h"
#include "base/strings/string_util.h"

namespace mitto {
namespace {

constexpr uint64_t kRapidSeed = 0xbdd89aa982704029ull;
constexpr uint64_t kRapidSecret[3] = {
    0x2d358dccaa6c78a5ull, 0x8bb84b93962eacc9ull, 0x4b33a62ed433d4a3ull};

// HashTable::kMaxLoad and HashTraits::kMinimumTableSize.
constexpr size_t kMaxLoad = 2;
constexpr size_t kMinimumTableSize = 8;

std::pair<uint64_t, uint64_t> RapidMul128(uint64_t a, uint64_t b) {
  const unsigned __int128 product = static_cast<unsigned __int128>(a) * b;
  return {static_cast<uint64_t>(product), static_cast<uint64_t>(product >> 64)};
}

uint64_t RapidMix(uint64_t a, uint64_t b) {
  const auto [low, high] = RapidMul128(a, b);
  return low ^ high;
}

// What CaseFoldingHashReader<LChar> reads: each character as a UTF-16 code
// unit, after case folding, which a header name only needs for ASCII. The
// upstream offsets are in bytes of that output, so these take them as
// characters.
uint64_t FoldCase(char character) {
  return static_cast<uint8_t>(base::ToLowerASCII(character));
}

uint64_t Read32(std::string_view name, size_t index) {
  return FoldCase(name[index]) | (FoldCase(name[index + 1]) << 16);
}

uint64_t Read64(std::string_view name, size_t index) {
  return Read32(name, index) | (Read32(name, index + 2) << 32);
}

// rapidhash_internal() with kExpansionFactor 2, for a name of at least two
// characters. A header name that came through a Persona always is one.
uint64_t RapidHash(std::string_view name) {
  CHECK_GE(name.size(), 2u);
  const size_t length = name.size() * 2;
  uint64_t seed = kRapidSeed ^
                  RapidMix(kRapidSeed ^ kRapidSecret[0], kRapidSecret[1]) ^
                  length;
  uint64_t a;
  uint64_t b;
  if (length <= 16) {
    const size_t last = (length - 4) / 2;
    const size_t delta = ((length & 24) >> (length >> 3)) / 2;
    a = (Read32(name, 0) << 32) | Read32(name, last);
    b = (Read32(name, delta) << 32) | Read32(name, last - delta);
  } else {
    size_t position = 0;
    size_t remaining = length;
    if (remaining > 48) {
      uint64_t see1 = seed;
      uint64_t see2 = seed;
      do {
        seed = RapidMix(Read64(name, position) ^ kRapidSecret[0],
                        Read64(name, position + 4) ^ seed);
        see1 = RapidMix(Read64(name, position + 8) ^ kRapidSecret[1],
                        Read64(name, position + 12) ^ see1);
        see2 = RapidMix(Read64(name, position + 16) ^ kRapidSecret[2],
                        Read64(name, position + 20) ^ see2);
        position += 24;
        remaining -= 48;
      } while (remaining >= 48);
      seed ^= see1 ^ see2;
    }
    if (remaining > 16) {
      seed = RapidMix(Read64(name, position) ^ kRapidSecret[2],
                      Read64(name, position + 4) ^ seed ^ kRapidSecret[1]);
      if (remaining > 32) {
        seed = RapidMix(Read64(name, position + 8) ^ kRapidSecret[2],
                        Read64(name, position + 12) ^ seed);
      }
    }
    // Upstream reads from the end of what is left, which can go back over what
    // the loop above has read.
    a = Read64(name, position + remaining / 2 - 8);
    b = Read64(name, position + remaining / 2 - 4);
  }
  a ^= kRapidSecret[1];
  b ^= seed;
  std::tie(a, b) = RapidMul128(a, b);
  return RapidMix(a ^ kRapidSecret[0] ^ length, b ^ kRapidSecret[1]);
}

// HashString24() with DeprecatedCaseFoldingHash's reader.
uint32_t HashString24(std::string_view name) {
  const uint32_t hash = RapidHash(name) & ((1u << 24) - 1);
  return hash ? hash : 0x80000000u >> 8;
}

using Buckets = std::vector<std::optional<std::string_view>>;

void Insert(Buckets& buckets, std::string_view name) {
  const size_t mask = buckets.size() - 1;
  size_t index = HashString24(name) & mask;
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
