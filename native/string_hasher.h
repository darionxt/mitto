// Blink's hash of a string, which its hash tables and StringImpl::GetHash()
// use. The source export leaves Blink out, so this is ported from
// third_party/blink/renderer/platform/wtf/text/string_hasher.h,
// .../wtf/text/case_folding_hash.h and third_party/rapidhash/rapidhash.h.

#ifndef MITTO_STRING_HASHER_H_
#define MITTO_STRING_HASHER_H_

#include <stdint.h>

#include <string_view>

namespace mitto {

// HashString24() with PlainHashReader: what StringImpl::GetHash() gives a
// string of 8-bit characters, such as the host of a URL.
uint32_t HashString24(std::string_view data);

// HashString24() with CaseFoldingHashReader<LChar>: what
// DeprecatedCaseFoldingHash gives a header name. It reads each character as a
// UTF-16 code unit after case folding, which a header name only needs for
// ASCII.
uint32_t CaseFoldingHashString24(std::string_view name);

}  // namespace mitto

#endif  // MITTO_STRING_HASHER_H_
