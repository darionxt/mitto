#include "mitto/dom/permissions_policy_parser.h"

#include <stddef.h>

#include <algorithm>
#include <iterator>
#include <utility>

#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"

namespace mitto {

namespace {

constexpr size_t kMaxLength = 1 << 16;

const PolicyFeature* Find(std::string_view name) {
  const base::span<const PolicyFeature> features = PolicyFeatures();
  const auto found = std::ranges::find(features, name, &PolicyFeature::name);
  return found == features.end() ? nullptr : &*found;
}

// ParseFeatureName().
const PolicyFeature* ParseFeature(std::string_view name,
                                  const bindings::RuntimeFeatureSet& features,
                                  std::vector<PolicyMessage>& messages) {
  const PolicyFeature* const feature = Find(name);
  const bool enabled =
      feature && (!feature->feature || features.IsEnabled(*feature->feature));
  if (!feature ||
      (feature->gate == PolicyFeature::Gate::kRuntimeFeature && !enabled)) {
    messages.push_back(
        {.text = base::StrCat({"Unrecognized feature: '", name, "'."})});
    return nullptr;
  }
  if (!enabled) {
    messages.push_back(
        {.text = base::StrCat(
             {"Origin trial controlled feature not enabled: '", name, "'."})});
    return nullptr;
  }
  return feature;
}

// ParsePolicyFromNode(), in which only the first declaration of a feature
// counts.
ParsedPolicy ParseNode(const PolicyNode& node,
                       const url::Origin& self,
                       const url::Origin* source,
                       bool with_wildcards,
                       const bindings::RuntimeFeatureSet& features,
                       std::vector<PolicyMessage>& messages) {
  std::ranges::copy(node.messages, std::back_inserter(messages));
  ParsedPolicy policy;
  for (const PolicyEntry& entry : node.entries) {
    const PolicyFeature* const feature =
        ParseFeature(entry.feature, features, messages);
    if (!feature) {
      continue;
    }
    PolicyAllowlist allowlist = ParsePolicyAllowlist(entry.tokens, self, source,
                                                     with_wildcards, messages);
    if (!FindDeclared(policy, *feature)) {
      policy.emplace_back(*feature, std::move(allowlist));
    }
  }
  return policy;
}

}  // namespace

ParsedPolicy ParseAllowAttribute(std::u16string_view allow,
                                 const url::Origin& self,
                                 const url::Origin& source,
                                 const bindings::RuntimeFeatureSet& features,
                                 std::vector<PolicyMessage>& messages) {
  // One that is too long is not even copied.
  const PolicyNode node = ParseFeaturePolicy(
      allow.size() > kMaxLength ? std::string() : base::UTF16ToUTF8(allow),
      allow.size(), /*is_header=*/false);
  return ParseNode(node, self, &source, /*with_wildcards=*/false, features,
                   messages);
}

ParsedPolicy ParsePolicyHeaders(const PermissionsPolicyHeaders& headers,
                                const url::Origin& self,
                                const bindings::RuntimeFeatureSet& features,
                                std::vector<PolicyMessage>& messages) {
  std::vector<PolicyMessage> of_feature_policy;
  std::vector<PolicyMessage> of_permissions_policy;
  ParsedPolicy policy =
      ParseNode(headers.permissions_policy, self, /*source=*/nullptr,
                /*with_wildcards=*/true, features, of_permissions_policy);
  std::vector<std::string_view> overlaps;
  for (PolicyDeclaration& declaration :
       ParseNode(headers.feature_policy, self, /*source=*/nullptr,
                 /*with_wildcards=*/false, features, of_feature_policy)) {
    if (FindDeclared(policy, *declaration.feature)) {
      overlaps.push_back(declaration.feature->name);
    } else {
      policy.push_back(std::move(declaration));
    }
  }
  if (!overlaps.empty()) {
    of_feature_policy.push_back(
        {.text = base::StrCat({"Some features are specified in both "
                               "Feature-Policy and Permissions-Policy header: ",
                               base::JoinString(overlaps, ", "),
                               ". Values defined in Permissions-Policy header "
                               "will be used."})});
  }
  for (PolicyMessage& message : of_feature_policy) {
    message.text =
        base::StrCat({"Error with Feature-Policy header: ", message.text});
    messages.push_back(std::move(message));
  }
  for (PolicyMessage& message : of_permissions_policy) {
    message.text =
        base::StrCat({"Error with Permissions-Policy header: ", message.text});
    messages.push_back(std::move(message));
  }
  return policy;
}

bool AllowEverywhereIfNotPresent(std::string_view feature,
                                 ParsedPolicy& policy) {
  const PolicyFeature& named = PolicyFeatureNamed(feature);
  if (FindDeclared(policy, named)) {
    return false;
  }
  policy.emplace_back(
      named, PolicyAllowlist{.all_origins = true, .opaque_source = true});
  return true;
}

void DisallowIfNotPresent(std::string_view feature, ParsedPolicy& policy) {
  const PolicyFeature& named = PolicyFeatureNamed(feature);
  if (!FindDeclared(policy, named)) {
    policy.emplace_back(named, PolicyAllowlist());
  }
}

}  // namespace mitto
