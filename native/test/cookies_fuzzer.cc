// Feeds any bytes to the code that reads cookies and the filter that deletes
// them. They come from outside the library, through
// mitto_session_set_cookies() and mitto_session_delete_cookies(), and from a
// file that a Session has saved.

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <string_view>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "base/compiler_specific.h"
#include "mitto/cookie_json.h"
#include "net/cookies/canonical_cookie.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // SAFETY: libFuzzer says `data` holds `size` bytes.
  const std::string_view json = UNSAFE_BUFFERS(
      std::string_view(reinterpret_cast<const char*>(data), size));
  mitto::ParseCookieFilter(json);

  const std::optional<std::vector<mitto::CookieToSet>> cookies =
      mitto::ParseCookies(json);
  if (!cookies) {
    return 0;
  }

  net::CookieList list;
  for (const mitto::CookieToSet& cookie : *cookies) {
    // What the cookie store relies on in a cookie that it is given, which it
    // does not check for itself, and what lets it replace a Secure cookie.
    CHECK(cookie.cookie->IsCanonical());
    CHECK(cookie.source_url.is_valid());
    CHECK(cookie.source_url.SchemeIsCryptographic());
    CHECK(cookie.cookie->IsDomainMatch(cookie.source_url.host()));
    list.push_back(*cookie.cookie);
  }

  // What was read can be written and read again, with nothing lost.
  const std::optional<std::vector<mitto::CookieToSet>> written =
      mitto::ParseCookies(mitto::WriteCookies(list));
  CHECK(written);
  CHECK_EQ(written->size(), list.size());
  for (size_t i = 0; i < list.size(); ++i) {
    CHECK((*written)[i].cookie->HasEquivalentDataMembers(list[i]));
  }
  return 0;
}
