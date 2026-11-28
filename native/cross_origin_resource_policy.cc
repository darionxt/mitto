#include "mitto/cross_origin_resource_policy.h"

#include <optional>
#include <string>

#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "url/url_constants.h"

namespace mitto {

namespace {

// CrossOriginResourcePolicy::ParsedHeader.
enum class ParsedHeader {
  kNoHeader,
  kSameOrigin,
  kSameSite,
  kCrossOrigin,
  kParsingError,
};

// ParseHeaderByString(): the value as it is, which two lines of the header,
// joined, never are.
ParsedHeader ParseHeaderByString(const std::optional<std::string>& value) {
  if (!value) {
    return ParsedHeader::kNoHeader;
  }
  if (*value == "same-origin") {
    return ParsedHeader::kSameOrigin;
  }
  if (*value == "same-site") {
    return ParsedHeader::kSameSite;
  }
  if (*value == "cross-origin") {
    return ParsedHeader::kCrossOrigin;
  }
  return ParsedHeader::kParsingError;
}

std::string GetDomain(const url::Origin& origin) {
  return net::registry_controlled_domains::GetDomainAndRegistry(
      origin, net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
}

// ShouldAllowSameSite(): the same registrable domain, and no step down from
// HTTPS to HTTP, where neither host is an IP address.
bool ShouldAllowSameSite(const url::Origin& initiator,
                         const url::Origin& target_origin) {
  if (initiator.GetURL().HostIsIPAddress() ||
      target_origin.GetURL().HostIsIPAddress()) {
    return false;
  }
  if (GetDomain(initiator) != GetDomain(target_origin)) {
    return false;
  }
  return initiator.scheme() == url::kHttpsScheme ||
         target_origin.scheme() != url::kHttpsScheme;
}

}  // namespace

std::optional<BlockedByResourcePolicy> IsBlockedByResourcePolicy(
    const GURL& request_url,
    const std::optional<url::Origin>& request_initiator,
    const std::optional<std::string>& corp_header_value,
    ResourcePolicyMode mode,
    bool request_include_credentials,
    const CrossOriginEmbedderPolicy& embedder_policy,
    const DocumentIsolationPolicy& document_isolation_policy) {
  // IsBlockedInternal().
  if (!request_initiator) {
    return std::nullopt;
  }
  const url::Origin& initiator = *request_initiator;
  bool require_corp_due_to_coep = false;
  switch (embedder_policy.value) {
    case CrossOriginEmbedderPolicyValue::kNone:
      break;
    case CrossOriginEmbedderPolicyValue::kCredentialless:
      require_corp_due_to_coep =
          mode == ResourcePolicyMode::kNavigate || request_include_credentials;
      break;
    case CrossOriginEmbedderPolicyValue::kRequireCorp:
      require_corp_due_to_coep = true;
      break;
  }
  bool require_corp_due_to_dip = false;
  switch (document_isolation_policy.value) {
    case DocumentIsolationPolicyValue::kNone:
      break;
    case DocumentIsolationPolicyValue::kIsolateAndCredentialless:
      require_corp_due_to_dip =
          mode == ResourcePolicyMode::kNavigate || request_include_credentials;
      break;
    case DocumentIsolationPolicyValue::kIsolateAndRequireCorp:
      require_corp_due_to_dip = true;
      break;
  }
  ParsedHeader policy = ParseHeaderByString(corp_header_value);
  // A response with no policy that COEP or DIP requires one of is held to
  // same-origin.
  bool upgrade_to_same_origin_due_to_coep = false;
  bool upgrade_to_same_origin_due_to_dip = false;
  if ((policy == ParsedHeader::kNoHeader ||
       policy == ParsedHeader::kParsingError) &&
      (require_corp_due_to_coep || require_corp_due_to_dip)) {
    policy = ParsedHeader::kSameOrigin;
    upgrade_to_same_origin_due_to_coep = require_corp_due_to_coep;
    upgrade_to_same_origin_due_to_dip = require_corp_due_to_dip;
  }
  if (policy == ParsedHeader::kNoHeader ||
      policy == ParsedHeader::kParsingError ||
      policy == ParsedHeader::kCrossOrigin) {
    return std::nullopt;
  }
  const url::Origin target_origin = url::Origin::Create(request_url);
  if (initiator == target_origin) {
    return std::nullopt;
  }
  if (policy == ParsedHeader::kSameOrigin) {
    if (upgrade_to_same_origin_due_to_coep &&
        upgrade_to_same_origin_due_to_dip) {
      return BlockedByResourcePolicy::
          kCorpNotSameOriginAfterDefaultedToSameOriginByCoepAndDip;
    }
    if (upgrade_to_same_origin_due_to_coep) {
      return BlockedByResourcePolicy::
          kCorpNotSameOriginAfterDefaultedToSameOriginByCoep;
    }
    if (upgrade_to_same_origin_due_to_dip) {
      return BlockedByResourcePolicy::
          kCorpNotSameOriginAfterDefaultedToSameOriginByDip;
    }
    return BlockedByResourcePolicy::kCorpNotSameOrigin;
  }
  if (ShouldAllowSameSite(initiator, target_origin)) {
    return std::nullopt;
  }
  return BlockedByResourcePolicy::kCorpNotSameSite;
}

}  // namespace mitto
