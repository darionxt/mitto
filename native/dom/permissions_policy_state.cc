#include "mitto/dom/permissions_policy_state.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/strings/utf_string_conversions.h"

namespace mitto {

namespace {

constexpr std::string_view kUnload = "unload";

size_t IndexOf(const PolicyFeature& feature) {
  return static_cast<size_t>(&feature - PolicyFeatures().data());
}

// CreateAllowlistsAndReportingEndpoints(): local-network-access says what it
// says of the two that it names too, but of one that is declared before it.
ParsedPolicy WithLocalNetworks(ParsedPolicy policy) {
  ParsedPolicy result;
  const auto add = [&](const PolicyFeature& feature,
                       const PolicyAllowlist& allowlist) {
    if (!FindDeclared(result, feature)) {
      result.emplace_back(feature, allowlist);
    }
  };
  for (const PolicyDeclaration& declaration : policy) {
    add(*declaration.feature, declaration.allowlist);
    if (declaration.feature->name == "local-network-access") {
      add(PolicyFeatureNamed("local-network"), declaration.allowlist);
      add(PolicyFeatureNamed("loopback-network"), declaration.allowlist);
    }
  }
  return result;
}

}  // namespace

PolicyDeclaration::PolicyDeclaration(const PolicyFeature& feature,
                                     PolicyAllowlist allowlist)
    : feature(feature), allowlist(std::move(allowlist)) {}
PolicyDeclaration::PolicyDeclaration(const PolicyDeclaration&) = default;
PolicyDeclaration::PolicyDeclaration(PolicyDeclaration&&) = default;
PolicyDeclaration& PolicyDeclaration::operator=(const PolicyDeclaration&) =
    default;
PolicyDeclaration& PolicyDeclaration::operator=(PolicyDeclaration&&) = default;
PolicyDeclaration::~PolicyDeclaration() = default;

const PolicyAllowlist* FindDeclared(const ParsedPolicy& policy,
                                    const PolicyFeature& feature) {
  const auto found =
      std::ranges::find_if(policy, [&](const PolicyDeclaration& declaration) {
        return &*declaration.feature == &feature;
      });
  return found == policy.end() ? nullptr : &found->allowlist;
}

PermissionsPolicyState::PermissionsPolicyState(
    const PermissionsPolicyState* parent,
    const ParsedPolicy& container_policy,
    const url::Origin& origin,
    bool unload_deprecated,
    ParsedPolicy declared)
    : origin_(origin),
      unload_deprecated_(unload_deprecated),
      declared_(WithLocalNetworks(std::move(declared))) {
  const base::span<const PolicyFeature> features = PolicyFeatures();
  CHECK_LE(features.size(), kMaxFeatures);
  for (const PolicyFeature& feature : features) {
    // InheritedValueForFeature().
    bool inherited = true;
    if (parent) {
      const PolicyAllowlist* const container =
          FindDeclared(container_policy, feature);
      inherited =
          parent->HasValueFor(feature, parent->origin_, IsForNone(feature)) &&
          parent->HasValueFor(feature, origin, IsForNone(feature)) &&
          (container ? container->Contains(origin)
                     : feature.default_allowlist ==
                               PolicyFeature::Default::kEnableForAll ||
                           origin.IsSameOriginWith(parent->origin_));
    }
    if (inherited) {
      inherited_.set(IndexOf(feature));
    }
  }
  // What local-network-access names in two.
  if (Inherits(PolicyFeatureNamed("local-network-access"))) {
    inherited_.set(IndexOf(PolicyFeatureNamed("local-network")));
    inherited_.set(IndexOf(PolicyFeatureNamed("loopback-network")));
  }
}

PermissionsPolicyState::~PermissionsPolicyState() = default;

bool PermissionsPolicyState::IsEnabledFor(const PolicyFeature& feature,
                                          const url::Origin& origin) const {
  if (!Inherits(feature)) {
    return false;
  }
  if (const PolicyAllowlist* const declared =
          FindDeclared(declared_, feature)) {
    return declared->Contains(origin);
  }
  return !IsForNone(feature) &&
         (feature.default_allowlist == PolicyFeature::Default::kEnableForAll ||
          origin_.IsSameOriginWith(origin));
}

bool PermissionsPolicyState::HasValueFor(const PolicyFeature& feature,
                                         const url::Origin& origin,
                                         bool is_for_none) const {
  if (!Inherits(feature)) {
    return false;
  }
  const PolicyAllowlist* const declared = FindDeclared(declared_, feature);
  return declared ? declared->Contains(origin) : !is_for_none;
}

std::vector<std::u16string> PermissionsPolicyState::AllowlistFor(
    const PolicyFeature& feature) const {
  if (!Inherits(feature)) {
    return {};
  }
  if (const PolicyAllowlist* const declared =
          FindDeclared(declared_, feature)) {
    if (declared->all_origins && declared->origins.empty()) {
      return {u"*"};
    }
    std::vector<std::u16string> origins;
    if (declared->self) {
      origins.push_back(base::UTF8ToUTF16(declared->self->Serialize()));
    }
    for (const PolicyOrigin& origin : declared->origins) {
      origins.push_back(base::UTF8ToUTF16(origin.Serialize()));
    }
    return origins;
  }
  if (IsForNone(feature)) {
    return {};
  }
  if (feature.default_allowlist == PolicyFeature::Default::kEnableForAll) {
    return {u"*"};
  }
  // OriginWithPossibleWildcards::FromOrigin() has none of an opaque origin, or
  // of one that a source cannot say, as of an IPv6 address.
  const std::optional<PolicyOrigin> origin =
      PolicyOrigin::Parse(origin_.Serialize(), /*with_wildcards=*/true);
  if (!origin) {
    return {};
  }
  return {base::UTF8ToUTF16(origin->Serialize())};
}

bool PermissionsPolicyState::IsForNone(const PolicyFeature& feature) const {
  return unload_deprecated_ && feature.name == kUnload;
}

bool PermissionsPolicyState::Inherits(const PolicyFeature& feature) const {
  return inherited_.test(IndexOf(feature));
}

}  // namespace mitto
