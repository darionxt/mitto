#include "mitto/dom/storage_key.h"

#include <algorithm>

#include "base/strings/strcat.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "url/gurl.h"

namespace mitto {

namespace {

// net::SchemefulSite::Serialize() of an origin that is not opaque: its scheme
// and registrable domain, or its host where it has none.
std::string SiteOf(const url::Origin& origin) {
  const std::string domain =
      net::registry_controlled_domains::GetDomainAndRegistry(
          origin, net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
  return base::StrCat(
      {origin.scheme(), "://", domain.empty() ? origin.host() : domain});
}

// An opaque origin's nonce, which it has once it is copied.
std::string NonceOf(const url::Origin& origin) {
  const url::Origin copy = origin;
  return copy.GetDebugString();
}

}  // namespace

std::string SerializeStorageKey(const url::Origin& origin,
                                const std::vector<url::Origin>& ancestors) {
  if (origin.opaque()) {
    return base::StrCat({"^1", NonceOf(origin)});
  }
  const std::string url = origin.GetURL().spec();
  if (ancestors.empty()) {
    return url;
  }
  const url::Origin& top = ancestors.back();
  if (top.opaque()) {
    return base::StrCat({url, "^4", NonceOf(top)});
  }
  const std::string site = SiteOf(origin);
  const std::string top_site = SiteOf(top);
  const bool is_cross_site =
      site != top_site ||
      std::ranges::any_of(ancestors, [&top_site](const url::Origin& ancestor) {
        return ancestor.opaque() || SiteOf(ancestor) != top_site;
      });
  if (!is_cross_site) {
    return url;
  }
  return site == top_site ? base::StrCat({url, "^31"})
                          : base::StrCat({url, "^0", top_site});
}

}  // namespace mitto
