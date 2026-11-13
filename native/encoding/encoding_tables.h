// The Encoding Standard's indexes of the CJK encodings, which Blink builds from
// ICU's converters the first time a decoder needs one, with the entries that
// ICU lacks or maps otherwise. Only what decoding reads is here.
//
// Upstream: third_party/blink/renderer/platform/wtf/text/encoding_tables.h
// Upstream: third_party/blink/renderer/platform/wtf/text/encoding_tables.cc

#ifndef MITTO_ENCODING_ENCODING_TABLES_H_
#define MITTO_ENCODING_ENCODING_TABLES_H_

#include <stdint.h>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace mitto {

// Each pointer that has a code point, in order of pointers.
using CodePointIndex = std::vector<std::pair<uint16_t, char16_t>>;

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

}  // namespace mitto

#endif  // MITTO_ENCODING_ENCODING_TABLES_H_
