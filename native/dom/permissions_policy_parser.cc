#include "mitto/dom/permissions_policy_parser.h"

#include <stddef.h>

#include <algorithm>
#include <utility>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
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

bool IsDeclared(const PolicyFeature& feature, const ContainerPolicy& policy) {
  return std::ranges::any_of(policy, [&](const PolicyDeclaration& declaration) {
    return &*declaration.feature == &feature;
  });
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

// FromOrigin().
void AddOrigin(const url::Origin& origin, PolicyDeclaration& declaration) {
  if (std::optional<PolicyOrigin> parsed =
          PolicyOrigin::Parse(origin.Serialize())) {
    declaration.origins.push_back(*std::move(parsed));
  }
}

// ParseAllowlist().
PolicyDeclaration ParseAllowlist(const PolicyFeature& feature,
                                 base::span<const std::string_view> tokens,
                                 const url::Origin& self,
                                 const url::Origin& source,
                                 std::vector<PolicyMessage>& messages) {
  PolicyDeclaration declaration{.feature = base::raw_ref(feature)};
  // 'src', which a feature with no allowlist is for, is the opaque origin that
  // the frame's document is to have, if its URL gives it one.
  const auto add_source = [&] {
    if (source.opaque()) {
      declaration.opaque_source = true;
    } else {
      AddOrigin(source, declaration);
    }
  };
  if (tokens.empty()) {
    add_source();
  }
  for (const std::string_view token : tokens) {
    if (!base::IsStringASCII(token)) {
      messages.push_back({.text = "Non-ASCII characters in origin."});
    } else if (base::EqualsCaseInsensitiveASCII(token, "'self'")) {
      declaration.self = self;
    } else if (base::EqualsCaseInsensitiveASCII(token, "'src'")) {
      add_source();
    } else if (base::EqualsCaseInsensitiveASCII(token, "'none'")) {
      continue;
    } else if (token == "*") {
      declaration.all_origins = true;
      declaration.opaque_source = true;
    } else if (std::optional<PolicyOrigin> origin =
                   PolicyOrigin::Parse(token)) {
      declaration.origins.push_back(*std::move(origin));
    } else {
      messages.push_back(
          {.text = base::StrCat({"Unrecognized origin: '", token, "'."})});
    }
  }
  return declaration;
}

}  // namespace

ContainerPolicy ParseAllowAttribute(std::u16string_view allow,
                                    const url::Origin& self,
                                    const url::Origin& source,
                                    const bindings::RuntimeFeatureSet& features,
                                    std::vector<PolicyMessage>& messages) {
  ContainerPolicy policy;
  if (allow.size() > kMaxLength) {
    messages.push_back(
        {ConsoleLevel::kError,
         base::StrCat({"Feature policy declaration exceeds size limit(",
                       base::NumberToString(allow.size()), ">",
                       base::NumberToString(kMaxLength), ")"})});
    return policy;
  }
  const std::string utf8 = base::UTF16ToUTF8(allow);
  for (const std::string_view entry : base::SplitStringPiece(
           utf8, ";", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    const std::vector<std::string_view> tokens = base::SplitStringPiece(
        entry, " \t\n\f\r", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
    if (tokens.empty()) {
      continue;
    }
    const PolicyFeature* const feature =
        ParseFeature(tokens[0], features, messages);
    if (!feature) {
      continue;
    }
    PolicyDeclaration declaration = ParseAllowlist(
        *feature, base::span(tokens).subspan<1>(), self, source, messages);
    // Only the first declaration of a feature counts.
    if (!IsDeclared(*feature, policy)) {
      policy.push_back(std::move(declaration));
    }
  }
  return policy;
}

bool AllowEverywhereIfNotPresent(std::string_view feature,
                                 ContainerPolicy& policy) {
  const PolicyFeature& named = PolicyFeatureNamed(feature);
  if (IsDeclared(named, policy)) {
    return false;
  }
  policy.push_back({.feature = base::raw_ref(named),
                    .all_origins = true,
                    .opaque_source = true});
  return true;
}

}  // namespace mitto
