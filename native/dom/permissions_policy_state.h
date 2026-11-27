// What a document's permissions policy gives it, network::PermissionsPolicy:
// the features that it inherits from its parent's document, as its frame's
// element's container policy passes them on, for origins that each feature's
// default allowlist, or its response's headers, name.
//
// Upstream: services/network/public/cpp/permissions_policy/permissions_policy.cc

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
#include "mitto/policy_allowlist.h"
#include "url/origin.h"

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

// What a policy says of a feature, ParsedPermissionsPolicyDeclaration.
struct PolicyDeclaration {
  PolicyDeclaration(const PolicyFeature& feature, PolicyAllowlist allowlist);
  PolicyDeclaration(const PolicyDeclaration&);
  PolicyDeclaration(PolicyDeclaration&&);
  PolicyDeclaration& operator=(const PolicyDeclaration&);
  PolicyDeclaration& operator=(PolicyDeclaration&&);
  ~PolicyDeclaration();

  raw_ref<const PolicyFeature> feature;
  PolicyAllowlist allowlist;
};
using ParsedPolicy = std::vector<PolicyDeclaration>;

// What `policy` says of `feature`, or null.
const PolicyAllowlist* FindDeclared(const ParsedPolicy& policy,
                                    const PolicyFeature& feature);

class PermissionsPolicyState {
 public:
  // CreateFromParentPolicy(): of a document of `origin` under one of `parent`,
  // null for a top-level document, in a frame of `container_policy`, whose
  // response's headers say `declared`, and which is headerless if no response
  // makes it, as IsHeaderlessUrl() says.
  PermissionsPolicyState(const PermissionsPolicyState* parent,
                         const ParsedPolicy& container_policy,
                         const url::Origin& origin,
                         bool unload_deprecated,
                         bool is_headerless,
                         ParsedPolicy declared);
  ~PermissionsPolicyState();

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
  // GetFeatureValueForOrigin(), for a document of `origin` under this one's.
  bool HasValueFor(const PolicyFeature& feature,
                   const url::Origin& origin,
                   bool is_for_none) const;
  // GetPermissionsPolicyFeatureList(): the seed's kDeprecateUnload gives
  // unload no origin at all.
  bool IsForNone(const PolicyFeature& feature) const;

  const url::Origin origin_;
  const bool unload_deprecated_;
  const bool is_headerless_;
  std::bitset<kMaxFeatures> inherited_;
  const ParsedPolicy declared_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PERMISSIONS_POLICY_STATE_H_
