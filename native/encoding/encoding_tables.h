// The Encoding Standard's indexes of the CJK encodings, which Blink builds from
// ICU's converters the first time a decoder or an encoder needs one, with the
// entries that ICU lacks or maps otherwise: by pointer for decoders, and by
// code point for encoders.
//
// Upstream: third_party/blink/renderer/platform/wtf/text/encoding_tables.h
// Upstream: third_party/blink/renderer/platform/wtf/text/encoding_tables.cc

#ifndef MITTO_ENCODING_ENCODING_TABLES_H_
#define MITTO_ENCODING_ENCODING_TABLES_H_

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

#include "base/containers/span.h"

namespace mitto {

// Each pointer that has a code point, in order of pointers.
using CodePointIndex = std::vector<std::pair<uint16_t, char16_t>>;
// Each code point that has a pointer, with its pointer, in order of code
// points, and a code point's pointers in order of pointers.
using PointerIndex = std::vector<std::pair<char16_t, uint16_t>>;
// The same for index-big5, whose code points go beyond the Basic Multilingual
// Plane, with one pointer for each code point.
using Big5PointerIndex = std::vector<std::pair<int32_t, uint16_t>>;

// index-jis0208, which EUC-JP, ISO-2022-JP and Shift_JIS decode with.
const CodePointIndex& Jis0208Index();
// index-jis0212, which EUC-JP decodes with after 0x8F.
const CodePointIndex& Jis0212Index();
// index-euc-kr.
const CodePointIndex& EucKrIndex();
// index-gb18030, with a code point for each of its 23,940 pointers.
const std::vector<char16_t>& Gb18030Table();
// index-big5, with a code point for each of its 19,782 pointers, or 0 for a
// pointer that has none.
const std::vector<int32_t>& Big5Table();

// index-jis0208 by code point, which EUC-JP, ISO-2022-JP and Shift_JIS encode
// with.
const PointerIndex& Jis0208EncodeIndex();
// index-euc-kr by code point, where each code point has one pointer.
const PointerIndex& EucKrEncodeIndex();
// index-gb18030 by code point, which GBK and gb18030 encode with.
const PointerIndex& Gb18030EncodeIndex();
// index-big5 by code point, with the pointer that ICU's converter encodes each
// code point with, which is the Encoding Standard's where several pointers
// have the code point.
const Big5PointerIndex& Big5EncodeIndex();

// The code point of `pointer` in `index`, as Blink's FindFirstInSortedPairs().
inline std::optional<char16_t> FindCodePoint(const CodePointIndex& index,
                                             uint16_t pointer) {
  auto it = std::ranges::lower_bound(index, pointer, {},
                                     &std::pair<uint16_t, char16_t>::first);
  if (it == index.end() || pointer < it->first) {
    return std::nullopt;
  }
  return it->second;
}

// The first pointer of `code_point` in `index`, as Blink's
// FindFirstInSortedPairs(). A code point beyond the Basic Multilingual Plane
// has none, which Blink checks before it searches.
inline std::optional<uint16_t> FindPointer(const PointerIndex& index,
                                           int32_t code_point) {
  if (code_point < 0 || code_point > 0xFFFF) {
    return std::nullopt;
  }
  const auto key = static_cast<char16_t>(code_point);
  auto it = std::ranges::lower_bound(index, key, {},
                                     &std::pair<char16_t, uint16_t>::first);
  if (it == index.end() || key < it->first) {
    return std::nullopt;
  }
  return it->second;
}

// The pointer of `code_point` in `index`, as Blink's FindFirstInSortedPairs().
inline std::optional<uint16_t> FindPointer(const Big5PointerIndex& index,
                                           int32_t code_point) {
  auto it = std::ranges::lower_bound(index, code_point, {},
                                     &std::pair<int32_t, uint16_t>::first);
  if (it == index.end() || code_point < it->first) {
    return std::nullopt;
  }
  return it->second;
}

// Every pointer of `code_point` in `index`, in order, as Blink's
// FindInSortedPairs().
inline base::span<const std::pair<char16_t, uint16_t>> FindPointers(
    const PointerIndex& index,
    int32_t code_point) {
  if (code_point < 0 || code_point > 0xFFFF) {
    return {};
  }
  const auto range =
      std::ranges::equal_range(index, static_cast<char16_t>(code_point), {},
                               &std::pair<char16_t, uint16_t>::first);
  return base::span(index).subspan(
      static_cast<size_t>(std::distance(index.begin(), range.begin())),
      static_cast<size_t>(range.size()));
}

}  // namespace mitto

#endif  // MITTO_ENCODING_ENCODING_TABLES_H_
