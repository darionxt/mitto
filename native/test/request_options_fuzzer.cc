// Feeds any bytes to the code that reads a request's options. They come from
// outside the library, through mitto_request_start().

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <string_view>

#include "base/check.h"
#include "base/compiler_specific.h"
#include "base/time/time.h"
#include "mitto/request_options.h"
#include "net/base/request_priority.h"
#include "net/http/http_util.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // SAFETY: libFuzzer says `data` holds `size` bytes.
  const std::string_view json = UNSAFE_BUFFERS(
      std::string_view(reinterpret_cast<const char*>(data), size));
  const std::optional<mitto::RequestOptions> options =
      mitto::ParseRequestOptions(json);
  if (!options) {
    return 0;
  }

  // What a request relies on in options that were accepted. //net writes the
  // method into the request line as it is, takes a priority as an index, and
  // would silently send no Referer for a URL that it cannot send as one. A timer
  // that cannot wait would end the request at once.
  CHECK(net::HttpUtil::IsToken(options->method));
  CHECK_GE(options->priority, net::MINIMUM_PRIORITY);
  CHECK_LE(options->priority, net::MAXIMUM_PRIORITY);
  CHECK(options->referrer.is_empty() || options->referrer.is_valid());
  for (const std::optional<base::TimeDelta>& timeout :
       {options->connect_timeout, options->read_timeout,
        options->total_timeout}) {
    CHECK(!timeout || timeout->is_positive());
  }
  return 0;
}
