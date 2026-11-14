// Ported from services/network/sec_header_helpers.h in Chromium, which the
// source export leaves out.
//
// Upstream: services/network/sec_header_helpers.h

#ifndef MITTO_SEC_HEADER_HELPERS_H_
#define MITTO_SEC_HEADER_HELPERS_H_

#include <string_view>

#include "base/types/optional_ref.h"
#include "mitto/request_options.h"
#include "url/gurl.h"

namespace net {
struct RedirectInfo;
class URLRequest;
}  // namespace net

namespace mitto {

// Whether `name` is a line that a caller gave or removed, which no rule
// changes.
bool IsFixedHeader(const FetchParams& params, std::string_view name);

// What Sec-Fetch-Mode says of a mode.
std::string_view RequestModeToString(RequestMode mode);

// Removes the sec-ch- and sec-fetch- lines that are not a caller's.
void RemoveSecHeaders(net::URLRequest& request, const FetchParams& params);

// Sets the Fetch Metadata headers for the next hop of `request`: the one that
// `pending_redirect` leads to, or else its URL. As upstream, this only adds
// them for a potentially trustworthy URL. Sec-Fetch-Frame-Top and
// Sec-Fetch-Frame-Ancestors are behind features that are off by default
// upstream and in the Baselines, so they are not ported.
void SetFetchMetadataHeaders(
    net::URLRequest& request,
    const FetchParams& params,
    base::optional_ref<const net::RedirectInfo> pending_redirect);

// Removes the lines that a URL that is not potentially trustworthy must not
// get, when the request goes to one from one that is.
void MaybeRemoveSecHeaders(net::URLRequest& request,
                           const FetchParams& params,
                           const GURL& pending_redirect_url);

}  // namespace mitto

#endif  // MITTO_SEC_HEADER_HELPERS_H_
