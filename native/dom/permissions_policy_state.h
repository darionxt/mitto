// What a document's permissions policy gives it, network::PermissionsPolicy:
// the features that it inherits from its parent's document, as its frame's
// element's container policy passes them on, for origins that each feature's
// default allowlist names. A document's own header is a Known Gap.
//
// Upstream: services/network/public/cpp/permissions_policy/permissions_policy.cc
// Upstream: services/network/public/cpp/permissions_policy/origin_with_possible_wildcards.cc
// Upstream: services/network/public/cpp/content_security_policy/csp_source.cc
// Upstream: services/network/public/cpp/content_security_policy/content_security_policy.cc

#ifndef MITTO_DOM_PERMISSIONS_POLICY_STATE_H_
#define MITTO_DOM_PERMISSIONS_POLICY_STATE_H_

#include <stddef.h>

#include <bitset>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "base/memory/raw_ref.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/page_messages.h"
#include "url/origin.h"
#include "url/third_party/mozilla/url_parse.h"

namespace mitto {

// A policy-controlled feature.
struct PolicyFeature {
  // Its default allowlist, as PermissionsPolicyFeatureDefault has it.
  enum class Default { kEnableForAll, kEnableForSelf };
  // What a feature of the name map waits on, as the generated
  // GetDefaultFeatureNameMap() has it: nothing, a runtime-enabled feature,
  // which it is in the map only with, or an origin trial, which leaves it in
  // the map but out of what a page sees while its runtime-enabled feature is
  // off.
  enum class Gate { kNone, kRuntimeFeature, kOriginTrial };

  std::string_view name;
  Default default_allowlist;
  Gate gate;
  std::optional<bindings::RuntimeFeature> feature;
};

// Every feature that a window's name map can hold, and the one of a name that
// the map always holds.
base::span<const PolicyFeature> PolicyFeatures();
const PolicyFeature& PolicyFeatureNamed(std::string_view name);

// Whether the seed's kDeprecateUnload takes unload away from a document of
// `origin`, as Chrome's network service decides it.
bool IsUnloadDeprecated(const DeprecatedUnload& seed,
                        const url::Origin& origin);

// An origin of an allow attribute's, OriginWithPossibleWildcards.
struct PolicyOrigin {
  // The origin that `text` says with a scheme and a host, as ParseSource()
  // reads a source without wildcards, or none.
  static std::optional<PolicyOrigin> Parse(std::string_view text);

  // CheckCSPSource() of a permissions policy, which no upgrade matches.
  bool Matches(const url::Origin& origin) const;

  std::string scheme;
  std::string host;
  int port = url::PORT_UNSPECIFIED;
};

// What an allow attribute says of a feature,
// ParsedPermissionsPolicyDeclaration.
struct PolicyDeclaration {
  // Allowlist::Contains().
  bool Contains(const url::Origin& origin) const;

  raw_ref<const PolicyFeature> feature;
  std::vector<PolicyOrigin> origins;
  std::optional<url::Origin> self;
  bool all_origins = false;
  bool opaque_source = false;
};
using ContainerPolicy = std::vector<PolicyDeclaration>;

class PermissionsPolicyState {
 public:
  // CreateFromParentPolicy(): of a document of `origin` under one of `parent`,
  // null for a top-level document, in a frame of `container_policy`.
  PermissionsPolicyState(const PermissionsPolicyState* parent,
                         const ContainerPolicy& container_policy,
                         const url::Origin& origin,
                         bool unload_deprecated);

  // IsFeatureEnabled() and IsFeatureEnabledForOrigin().
  bool IsEnabled(const PolicyFeature& feature) const {
    return IsEnabledFor(feature, origin_);
  }
  bool IsEnabledFor(const PolicyFeature& feature,
                    const url::Origin& origin) const;
  // GetAllowlistForFeature(), as getAllowlistForFeature() shows it.
  std::vector<std::u16string> AllowlistFor(const PolicyFeature& feature) const;

 private:
  static constexpr size_t kMaxFeatures = 128;

  bool Inherits(const PolicyFeature& feature) const;
  // GetPermissionsPolicyFeatureList(): the seed's kDeprecateUnload gives
  // unload no origin at all.
  bool IsForNone(const PolicyFeature& feature) const;

  const url::Origin origin_;
  const bool unload_deprecated_;
  std::bitset<kMaxFeatures> inherited_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PERMISSIONS_POLICY_STATE_H_
