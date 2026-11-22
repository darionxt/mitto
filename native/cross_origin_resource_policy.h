// Cross-Origin-Resource-Policy, as Chrome's network service checks it on each
// response and redirect of a request of mode no-cors from another origin, with
// the embedder policy of the document or worker that asks. The source export
// leaves services/network out, so this is ported with the upstream names
// kept.
//
// Upstream: services/network/public/cpp/cross_origin_resource_policy.cc

#ifndef MITTO_CROSS_ORIGIN_RESOURCE_POLICY_H_
#define MITTO_CROSS_ORIGIN_RESOURCE_POLICY_H_

#include <optional>
#include <string>

#include "mitto/cross_origin_policies.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace mitto {

// network::mojom::BlockedByResponseReason, for what Cross-Origin-Resource-
// Policy blocks.
enum class BlockedByResourcePolicy {
  kCorpNotSameOrigin,
  kCorpNotSameOriginAfterDefaultedToSameOriginByCoep,
  kCorpNotSameSite,
};

// CrossOriginResourcePolicy::IsBlockedByHeaderValue() for a request of mode
// no-cors: whether the response at `request_url`, whose
// Cross-Origin-Resource-Policy is `corp_header_value`, nullopt for none, is
// blocked for a request from `request_initiator`, which sent credentials if
// `request_include_credentials`, of a client of embedder policy
// `embedder_policy`, and why. A request with no initiator, such as one that
// the browser starts, is never blocked. Reports, report-only ones included, go
// to endpoints that a Session never sends to.
std::optional<BlockedByResourcePolicy> IsBlockedByResourcePolicy(
    const GURL& request_url,
    const std::optional<url::Origin>& request_initiator,
    const std::optional<std::string>& corp_header_value,
    bool request_include_credentials,
    const CrossOriginEmbedderPolicy& embedder_policy);

}  // namespace mitto

#endif  // MITTO_CROSS_ORIGIN_RESOURCE_POLICY_H_
