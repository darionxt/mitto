#include "mitto/dom/permissions_policy_state.h"

#include <algorithm>

#include "base/check.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "url/gurl.h"
#include "url/url_util.h"

namespace mitto {

namespace {

constexpr std::string_view kUnload = "unload";

size_t IndexOf(const PolicyFeature& feature) {
  return static_cast<size_t>(&feature - PolicyFeatures().data());
}

// ParseScheme().
bool IsScheme(std::string_view text) {
  return !text.empty() && base::IsAsciiAlpha(text[0]) &&
         std::ranges::all_of(text, [](char c) {
           return base::IsAsciiAlphaNumeric(c) || c == '+' || c == '-' ||
                  c == '.';
         });
}

// ParseHost(), where only a trailing dot leaves an empty label.
bool IsHost(std::string_view text) {
  const std::vector<std::string_view> labels = base::SplitStringPiece(
      text, ".", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  for (size_t i = 0; i < labels.size(); ++i) {
    if ((labels[i].empty() && i + 1 < labels.size()) ||
        !std::ranges::all_of(labels[i], [](char c) {
          return base::IsAsciiAlphaNumeric(c) || c == '-';
        })) {
      return false;
    }
  }
  return !text.empty();
}

}  // namespace

// static
std::optional<PolicyOrigin> PolicyOrigin::Parse(std::string_view text) {
  const size_t scheme_end = text.find_first_of(":/");
  if (scheme_end == std::string_view::npos ||
      !text.substr(scheme_end).starts_with("://") ||
      !IsScheme(text.substr(0, scheme_end))) {
    return std::nullopt;
  }
  PolicyOrigin origin;
  origin.scheme = base::ToLowerASCII(text.substr(0, scheme_end));
  text = text.substr(scheme_end + 3);
  const size_t host_end = text.find_first_of(":/");
  if (!IsHost(text.substr(0, host_end))) {
    return std::nullopt;
  }
  origin.host = base::ToLowerASCII(text.substr(0, host_end));
  // The path that may follow the port says nothing of the origin.
  if (host_end != std::string_view::npos && text[host_end] == ':') {
    const std::string_view after = text.substr(host_end + 1);
    const std::string_view port = after.substr(0, after.find('/'));
    if (port.empty() || !std::ranges::all_of(port, &base::IsAsciiDigit<char>) ||
        !base::StringToInt(port, &origin.port)) {
      return std::nullopt;
    }
  }
  return origin;
}

bool PolicyOrigin::Matches(const url::Origin& origin) const {
  const GURL url = origin.GetURL();
  if (scheme != url.GetScheme() ||
      host != (url.IsStandard() ? url.GetHost() : std::string())) {
    return false;
  }
  // SourceAllowPort().
  const int default_port = url::DefaultPortForScheme(scheme);
  const int url_port = url.EffectiveIntPort();
  return port == url_port ||
         (port == url::PORT_UNSPECIFIED && default_port == url_port) ||
         (url_port == url::PORT_UNSPECIFIED && port == default_port);
}

bool PolicyDeclaration::Contains(const url::Origin& origin) const {
  if (all_origins || origin == self || (opaque_source && origin.opaque())) {
    return true;
  }
  return !origin.opaque() &&
         std::ranges::any_of(origins, [&](const PolicyOrigin& allowed) {
           return allowed.Matches(origin);
         });
}

PermissionsPolicyState::PermissionsPolicyState(
    const PermissionsPolicyState* parent,
    const ContainerPolicy& container_policy,
    const url::Origin& origin,
    bool unload_deprecated)
    : origin_(origin), unload_deprecated_(unload_deprecated) {
  const base::span<const PolicyFeature> features = PolicyFeatures();
  CHECK_LE(features.size(), kMaxFeatures);
  for (const PolicyFeature& feature : features) {
    // InheritedValueForFeature(), where a policy that declares nothing gives
    // each origin what it inherits, but what is for none.
    bool inherited =
        !parent || (parent->Inherits(feature) && !IsForNone(feature));
    if (parent && inherited) {
      const auto declared = std::ranges::find_if(
          container_policy, [&](const PolicyDeclaration& declaration) {
            return &*declaration.feature == &feature;
          });
      inherited = declared != container_policy.end()
                      ? declared->Contains(origin)
                      : feature.default_allowlist ==
                                PolicyFeature::Default::kEnableForAll ||
                            origin.IsSameOriginWith(parent->origin_);
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

bool PermissionsPolicyState::IsEnabledFor(const PolicyFeature& feature,
                                          const url::Origin& origin) const {
  return Inherits(feature) && !IsForNone(feature) &&
         (feature.default_allowlist == PolicyFeature::Default::kEnableForAll ||
          origin_.IsSameOriginWith(origin));
}

std::vector<std::u16string> PermissionsPolicyState::AllowlistFor(
    const PolicyFeature& feature) const {
  if (!Inherits(feature) || IsForNone(feature)) {
    return {};
  }
  if (feature.default_allowlist == PolicyFeature::Default::kEnableForAll) {
    return {u"*"};
  }
  // OriginWithPossibleWildcards::FromOrigin() has none of an opaque origin.
  if (origin_.opaque()) {
    return {};
  }
  return {base::UTF8ToUTF16(origin_.Serialize())};
}

bool PermissionsPolicyState::IsForNone(const PolicyFeature& feature) const {
  return unload_deprecated_ && feature.name == kUnload;
}

bool PermissionsPolicyState::Inherits(const PolicyFeature& feature) const {
  return inherited_.test(IndexOf(feature));
}

}  // namespace mitto
