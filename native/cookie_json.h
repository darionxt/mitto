#ifndef MITTO_COOKIE_JSON_H_
#define MITTO_COOKIE_JSON_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "net/cookies/canonical_cookie.h"
#include "url/gurl.h"

namespace mitto {

// A cookie to give to a cookie store, and the URL it counts as coming from.
struct CookieToSet {
  CookieToSet();
  CookieToSet(CookieToSet&&);
  CookieToSet& operator=(CookieToSet&&);
  ~CookieToSet();

  // Always canonical, and not null.
  std::unique_ptr<net::CanonicalCookie> cookie;
  // An HTTPS URL at the cookie's domain, so that the store lets it replace a
  // Secure cookie, whatever its own scheme was.
  GURL source_url;
};

// Which cookies to delete, as mitto_session_delete_cookies() describes.
struct CookieFilter {
  CookieFilter();
  CookieFilter(CookieFilter&&);
  CookieFilter& operator=(CookieFilter&&);
  ~CookieFilter();

  bool Matches(const net::CanonicalCookie& cookie) const;

  // Each has to equal that field of a cookie. nullopt matches any.
  std::optional<std::string> name;
  std::optional<std::string> domain;
  std::optional<std::string> path;
};

// Reads the JSON format described at mitto_session_set_cookies() in mitto.h.
// Returns nullopt if `json` is not valid in that format or holds a cookie that
// //net would not store. `json` comes from outside the library, so anything may
// be in it.
std::optional<std::vector<CookieToSet>> ParseCookies(std::string_view json);

// Reads the JSON format described at mitto_session_delete_cookies() in mitto.h.
// Returns nullopt if `json` is not valid in that format. `json` comes from
// outside the library, so anything may be in it.
std::optional<CookieFilter> ParseCookieFilter(std::string_view json);

// Writes the JSON format described at mitto_session_list_cookies() in mitto.h.
// Leaves out a cookie whose partition key cannot be written, as Chrome's own
// store does, because that key means nothing after the frame that made it.
std::string WriteCookies(const net::CookieList& cookies);

}  // namespace mitto

#endif  // MITTO_COOKIE_JSON_H_
