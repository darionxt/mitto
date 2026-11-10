// The hash below is rapidhash, Copyright 2024 Nicolas De Carli, under the BSD
// 2-Clause license in third_party/rapidhash/LICENSE of Chromium's source tree.
// The readers follow third_party/blink/renderer/platform/wtf, under the license
// in that directory.
//
// Upstream: third_party/rapidhash/rapidhash.h
// Upstream: third_party/blink/renderer/platform/wtf/text/string_hasher.h
// Upstream: third_party/blink/renderer/platform/wtf/text/case_folding_hash.h

#include "mitto/string_hasher.h"

#include <stddef.h>
#include <stdint.h>

#include <string_view>
#include <tuple>
#include <utility>

#include "base/check_op.h"
#include "base/containers/span.h"
#include "base/numerics/byte_conversions.h"
#include "base/strings/string_util.h"

namespace mitto {
namespace {

constexpr uint64_t kRapidSeed = 0xbdd89aa982704029ull;
constexpr uint64_t kRapidSecret[3] = {
    0x2d358dccaa6c78a5ull, 0x8bb84b93962eacc9ull, 0x4b33a62ed433d4a3ull};

// Upstream readers take a pointer into the input. These take the input and an
// offset into it, in characters, which are its bytes here.

// PlainHashReader, which reads the bytes as they are.
struct PlainHashReader {
  static constexpr size_t kCompressionFactor = 1;
  static constexpr size_t kExpansionFactor = 1;

  static uint64_t Read64(std::string_view data, size_t index) {
    return base::U64FromLittleEndian(
        base::as_byte_span(data).subspan(index).first<8u>());
  }

  static uint64_t Read32(std::string_view data, size_t index) {
    return base::U32FromLittleEndian(
        base::as_byte_span(data).subspan(index).first<4u>());
  }

  static uint64_t ReadSmall(std::string_view data, size_t k) {
    return (uint64_t{Byte(data, 0)} << 56) |
           (uint64_t{Byte(data, k >> 1)} << 32) | Byte(data, k - 1);
  }

 private:
  static uint8_t Byte(std::string_view data, size_t index) {
    return static_cast<uint8_t>(data[index]);
  }
};

// CaseFoldingHashReader<LChar>, which gives two bytes for each character.
struct CaseFoldingHashReader {
  static constexpr size_t kCompressionFactor = 1;
  static constexpr size_t kExpansionFactor = 2;

  static uint64_t Read64(std::string_view data, size_t index) {
    return Read32(data, index) | (Read32(data, index + 2) << 32);
  }

  static uint64_t Read32(std::string_view data, size_t index) {
    return FoldCase(data[index]) | (FoldCase(data[index + 1]) << 16);
  }

  static uint64_t ReadSmall(std::string_view data, size_t k) {
    DCHECK_EQ(k, 2u);
    return FoldCase(data[0]);
  }

 private:
  // Upstream folds Latin-1 by a table, which agrees with this for ASCII.
  static uint64_t FoldCase(char character) {
    return static_cast<uint8_t>(base::ToLowerASCII(character));
  }
};

std::pair<uint64_t, uint64_t> RapidMul128(uint64_t a, uint64_t b) {
  const unsigned __int128 product = static_cast<unsigned __int128>(a) * b;
  return {static_cast<uint64_t>(product), static_cast<uint64_t>(product >> 64)};
}

uint64_t RapidMix(uint64_t a, uint64_t b) {
  const auto [low, high] = RapidMul128(a, b);
  return low ^ high;
}

// rapidhash_internal(), where `len` is the number of bytes that the reader
// gives.
template <class Reader>
uint64_t RapidHashInternal(std::string_view data,
                           const size_t len,
                           uint64_t seed,
                           const uint64_t (&secret)[3]) {
  constexpr size_t x = Reader::kCompressionFactor;
  constexpr size_t y = Reader::kExpansionFactor;
  DCHECK_EQ(len % y, 0u);

  size_t p = 0;
  seed ^= RapidMix(seed ^ secret[0], secret[1]) ^ len;
  uint64_t a;
  uint64_t b;
  if (len <= 16) {
    if (len >= 4) {
      const size_t plast = p + (len - 4) * x / y;
      a = (Reader::Read32(data, p) << 32) | Reader::Read32(data, plast);
      const size_t delta = ((len & 24) >> (len >> 3)) * x / y;
      b = (Reader::Read32(data, p + delta) << 32) |
          Reader::Read32(data, plast - delta);
    } else if (len > 0) {
      a = Reader::ReadSmall(data, len);
      b = 0;
    } else {
      a = b = 0;
    }
  } else {
    size_t i = len;
    if (i > 48) {
      uint64_t see1 = seed;
      uint64_t see2 = seed;
      do {
        seed = RapidMix(Reader::Read64(data, p) ^ secret[0],
                        Reader::Read64(data, p + 8 * x / y) ^ seed);
        see1 = RapidMix(Reader::Read64(data, p + 16 * x / y) ^ secret[1],
                        Reader::Read64(data, p + 24 * x / y) ^ see1);
        see2 = RapidMix(Reader::Read64(data, p + 32 * x / y) ^ secret[2],
                        Reader::Read64(data, p + 40 * x / y) ^ see2);
        p += 48 * x / y;
        i -= 48;
      } while (i >= 48);
      seed ^= see1 ^ see2;
    }
    if (i > 16) {
      seed = RapidMix(Reader::Read64(data, p) ^ secret[2],
                      Reader::Read64(data, p + 8 * x / y) ^ seed ^ secret[1]);
      if (i > 32) {
        seed = RapidMix(Reader::Read64(data, p + 16 * x / y) ^ secret[2],
                        Reader::Read64(data, p + 24 * x / y) ^ seed);
      }
    }
    // Upstream reads from the end of what is left, which can go back over what
    // the loop above has read, so the offset from `p` can be below 0.
    const auto from_end = [p, i](ptrdiff_t back) {
      return static_cast<size_t>(static_cast<ptrdiff_t>(p) +
                                 (static_cast<ptrdiff_t>(i) - back) *
                                     static_cast<ptrdiff_t>(x) /
                                     static_cast<ptrdiff_t>(y));
    };
    a = Reader::Read64(data, from_end(16));
    b = Reader::Read64(data, from_end(8));
  }
  a ^= secret[1];
  b ^= seed;
  std::tie(a, b) = RapidMul128(a, b);
  return RapidMix(a ^ secret[0] ^ len, b ^ secret[1]);
}

// internal::MaskTop8Bits(), which leaves 8 bits for StringImpl's flags and
// never gives 0, which stands for a hash not worked out yet.
uint32_t MaskTop8Bits(uint64_t result) {
  constexpr uint32_t kFlagCount = 8;
  result &= (1u << (32 - kFlagCount)) - 1;
  if (!result) {
    result = 0x80000000u >> kFlagCount;
  }
  return static_cast<uint32_t>(result);
}

template <class Reader>
uint32_t HashString24WithReader(std::string_view data) {
  return MaskTop8Bits(RapidHashInternal<Reader>(
      data, data.size() * Reader::kExpansionFactor / Reader::kCompressionFactor,
      kRapidSeed, kRapidSecret));
}

}  // namespace

uint32_t HashString24(std::string_view data) {
  return HashString24WithReader<PlainHashReader>(data);
}

uint32_t CaseFoldingHashString24(std::string_view name) {
  return HashString24WithReader<CaseFoldingHashReader>(name);
}

}  // namespace mitto
