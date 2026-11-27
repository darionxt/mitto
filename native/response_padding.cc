#include "mitto/response_padding.h"

#include <array>
#include <string>

#include "base/containers/span.h"
#include "base/numerics/byte_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "crypto/hmac.h"
#include "crypto/random.h"
#include "mitto/page_messages.h"
#include "net/base/schemeful_site.h"

namespace mitto {

int64_t ComputeRandomResponsePadding() {
  uint64_t raw_random = 0;
  crypto::RandBytes(base::byte_span_from_ref(raw_random));
  return static_cast<int64_t>(raw_random % kResponsePaddingRange);
}

int64_t ComputeStableResponsePadding(const url::Origin& origin,
                                     std::string_view response_url,
                                     base::Time response_time,
                                     std::string_view request_method) {
  // It only has to be consistent within one process, so it is made the first
  // time.
  static const std::array<uint8_t, 16> padding_key =
      crypto::RandBytesAsArray<16>();
  const std::string key = base::StrCat(
      {response_url, "-",
       base::NumberToString(
           (response_time - base::Time::UnixEpoch()).InMicroseconds()),
       "-", net::SchemefulSite(origin).Serialize(), "-", request_method, "-0"});
  const std::array<uint8_t, 32> hmac =
      crypto::hmac::SignSha256(padding_key, base::as_byte_span(key));
  return static_cast<int64_t>(
      base::U64FromNativeEndian(base::as_byte_span(hmac).first<8>()) %
      kResponsePaddingRange);
}

}  // namespace mitto
