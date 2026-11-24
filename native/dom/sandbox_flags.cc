#include "mitto/dom/sandbox_flags.h"

#include <set>
#include <vector>

#include "base/containers/fixed_flat_map.h"
#include "base/strings/strcat.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"

namespace mitto {

namespace {

// What a token allows, in any case, or kNone.
SandboxFlags ParseSandboxToken(std::string_view token) {
  using enum SandboxFlags;
  static constexpr auto kTokens =
      base::MakeFixedFlatMap<std::string_view, SandboxFlags>({
          {"allow-downloads", kDownloads},
          {"allow-forms", kForms},
          {"allow-modals", kModals},
          {"allow-orientation-lock", kOrientationLock},
          {"allow-pointer-lock", kPointerLock},
          {"allow-popups", kPopups | kTopNavigationToCustomProtocols},
          {"allow-popups-to-escape-sandbox",
           kPropagatesToAuxiliaryBrowsingContexts},
          {"allow-presentation", kPresentationController},
          {"allow-same-origin", kOrigin},
          {"allow-same-site-none-cookies", kAllowSameSiteNoneCookies},
          {"allow-scripts", kAutomaticFeatures | kScripts},
          {"allow-storage-access-by-user-activation",
           kStorageAccessByUserActivation},
          {"allow-top-navigation",
           kTopNavigation | kTopNavigationToCustomProtocols},
          {"allow-top-navigation-by-user-activation",
           kTopNavigationByUserActivation},
          {"allow-top-navigation-to-custom-protocols",
           kTopNavigationToCustomProtocols},
      });
  const auto found = kTokens.find(base::ToLowerASCII(token));
  return found == kTokens.end() ? kNone : found->second;
}

}  // namespace

SandboxPolicy ParseSandboxPolicy(std::string_view input) {
  SandboxPolicy policy;
  std::vector<std::string_view> errors;
  std::set<std::string_view> seen;
  for (const std::string_view token :
       base::SplitStringPiece(input, " \n\t\r\f", base::KEEP_WHITESPACE,
                              base::SPLIT_WANT_NONEMPTY)) {
    const SandboxFlags allowed = ParseSandboxToken(token);
    policy.flags = policy.flags & ~allowed;
    if (allowed == SandboxFlags::kNone && seen.insert(token).second) {
      errors.push_back(token);
    }
  }
  if (!errors.empty()) {
    policy.error_message =
        base::StrCat({"'", base::JoinString(errors, "', '"),
                      errors.size() > 1 ? "' are invalid sandbox flags."
                                        : "' is an invalid sandbox flag."});
  }
  return policy;
}

}  // namespace mitto
