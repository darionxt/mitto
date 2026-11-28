#include "mitto/url_user_info.h"

namespace mitto {

UrlUserInfo UserInfoOf(const GURL& url) {
  return {.username = std::string(url.username()),
          .password = std::string(url.password())};
}

bool HasUserInfo(const GURL& url) {
  return !url.username().empty() || !url.password().empty();
}

GURL WithoutUserInfo(const GURL& url) {
  if (!HasUserInfo(url)) {
    return url;
  }
  GURL::Replacements replacements;
  replacements.ClearUsername();
  replacements.ClearPassword();
  return url.ReplaceComponents(replacements);
}

bool IsBlockedCredentialedSubresource(const GURL& url,
                                      const UrlUserInfo& client,
                                      const url::Origin& client_origin) {
  return HasUserInfo(url) && !(url.username() == client.username &&
                               url.password() == client.password &&
                               client_origin.IsSameOriginWith(url));
}

}  // namespace mitto
