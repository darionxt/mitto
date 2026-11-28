// The user and the password of a URL, and what Blink makes of a subresource's,
// which a Page's DOM checks as it asks for one, and the library on its
// redirects.
//
// Upstream: third_party/blink/renderer/core/loader/frame_fetch_context.cc
// Upstream: third_party/blink/renderer/core/loader/worker_fetch_context.cc
// Upstream: third_party/blink/renderer/platform/loader/fetch/fetch_parameters.cc

#ifndef MITTO_URL_USER_INFO_H_
#define MITTO_URL_USER_INFO_H_

#include <string>

#include "url/gurl.h"
#include "url/origin.h"

namespace mitto {

// The user and the password of a URL, as it has them, each empty for none.
struct UrlUserInfo {
  std::string username;
  std::string password;
};

UrlUserInfo UserInfoOf(const GURL& url);

// Whether `url` has a user or a password that is not empty.
bool HasUserInfo(const GURL& url);

// `url` without its user and its password, as
// ResourceRequestHead::RemoveUserAndPassFromURL() and
// KURL::StrippedForUseAsHref() take them off.
GURL WithoutUserInfo(const GURL& url);

// FetchContext::ShouldBlockFetchAsCredentialedSubresource(): whether a
// document or worker whose URL has `client`, of `client_origin`, refuses a
// subresource of `url`: one whose URL has a user or a password, unless both
// are `client` and it is of `client_origin`.
bool IsBlockedCredentialedSubresource(const GURL& url,
                                      const UrlUserInfo& client,
                                      const url::Origin& client_origin);

}  // namespace mitto

#endif  // MITTO_URL_USER_INFO_H_
