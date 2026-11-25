// The allowlists of permissions policies, as an allow attribute and a
// response's headers say them, which the Page and the library both read: what
// Blink's PermissionsPolicyParser makes of each feature's, and
// network::PermissionsPolicy::Allowlist.
//
// Upstream: third_party/blink/renderer/core/permissions_policy/permissions_policy_parser.cc
// Upstream: services/network/public/cpp/permissions_policy/permissions_policy.cc
// Upstream: services/network/public/cpp/permissions_policy/origin_with_possible_wildcards.cc
// Upstream: services/network/public/cpp/content_security_policy/csp_source.cc
// Upstream: services/network/public/cpp/content_security_policy/content_security_policy.cc

#ifndef MITTO_POLICY_ALLOWLIST_H_
#define MITTO_POLICY_ALLOWLIST_H_

#include <stddef.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "url/origin.h"
#include "url/third_party/mozilla/url_parse.h"

namespace mitto {

// What the console is told of a policy, PolicyParserMessageBuffer.
struct PolicyMessage {
  bool is_error = false;
  std::string text;
};

// An origin of an allowlist, OriginWithPossibleWildcards: one of an attribute
// or of a Feature-Policy header has no wildcards.
struct PolicyOrigin {
  static std::optional<PolicyOrigin> Parse(std::string_view text,
                                           bool with_wildcards);

  // CheckCSPSource() of a permissions policy, which no upgrade matches.
  bool Matches(const url::Origin& origin) const;
  // ToString() of a CSP source.
  std::string Serialize() const;

  // In the order of the fields of network::mojom::CSPSource.
  auto operator<=>(const PolicyOrigin&) const = default;

  std::string scheme;
  std::string host;
  int port = url::PORT_UNSPECIFIED;
  bool is_host_wildcard = false;
  bool is_port_wildcard = false;
};

struct PolicyAllowlist {
  // Allowlist::Contains().
  bool Contains(const url::Origin& origin) const;

  std::vector<PolicyOrigin> origins;
  std::optional<url::Origin> self;
  bool all_origins = false;
  bool opaque_source = false;
};

// ParseAllowlist() of `tokens`, of a policy of a document of `self`: in an
// attribute, with the origin of its frame's URL as `source`.
PolicyAllowlist ParsePolicyAllowlist(base::span<const std::string> tokens,
                                     const url::Origin& self,
                                     const url::Origin* source,
                                     bool with_wildcards,
                                     std::vector<PolicyMessage>& messages);

// What a policy says of a feature, PermissionsPolicyParser::Declaration: its
// name, whether or not there is such a feature, and its allowlist's tokens.
struct PolicyEntry {
  bool operator==(const PolicyEntry&) const = default;

  std::string feature;
  std::vector<std::string> tokens;
};

// A policy, PermissionsPolicyParser::Node, and what the console is told of its
// syntax.
struct PolicyNode {
  std::vector<PolicyMessage> messages;
  std::vector<PolicyEntry> entries;
};

// A response's Feature-Policy and Permissions-Policy headers.
struct PermissionsPolicyHeaders {
  bool empty() const {
    return feature_policy.messages.empty() && feature_policy.entries.empty() &&
           permissions_policy.messages.empty() &&
           permissions_policy.entries.empty();
  }

  // Whether they declare `feature` for a document of `self`, and if so whether
  // for `origin`, as ParseHeader() has it: what Permissions-Policy says, and
  // what Feature-Policy says of a feature that it does not. Null for neither.
  std::optional<bool> Allows(std::string_view feature,
                             const url::Origin& self,
                             const url::Origin& origin) const;

  PolicyNode feature_policy;
  PolicyNode permissions_policy;
};

// ParseFeaturePolicyToIR() of `policy`, in UTF-8, of `length` code units, which
// a header's commas part too.
PolicyNode ParseFeaturePolicy(std::string_view policy,
                              size_t length,
                              bool is_header);

}  // namespace mitto

#endif  // MITTO_POLICY_ALLOWLIST_H_
