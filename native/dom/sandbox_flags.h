// What the sandbox of a frame withholds from its documents:
// network::mojom::WebSandboxFlags, a bit for each, and the parser of an
// iframe's sandbox attribute, whose tokens each allow what they name.
//
// Upstream: services/network/public/mojom/web_sandbox_flags.mojom
// Upstream: services/network/public/cpp/web_sandbox_flags.cc

#ifndef MITTO_DOM_SANDBOX_FLAGS_H_
#define MITTO_DOM_SANDBOX_FLAGS_H_

#include <stdint.h>

#include <string>
#include <string_view>

namespace mitto {

enum class SandboxFlags : int32_t {
  kNone = 0,
  kNavigation = 1 << 0,
  kPlugins = 1 << 1,
  kOrigin = 1 << 2,
  kForms = 1 << 3,
  kScripts = 1 << 4,
  kTopNavigation = 1 << 5,
  kPopups = 1 << 6,
  kAutomaticFeatures = 1 << 7,
  kPointerLock = 1 << 8,
  kDocumentDomain = 1 << 9,
  kOrientationLock = 1 << 10,
  kPropagatesToAuxiliaryBrowsingContexts = 1 << 11,
  kModals = 1 << 12,
  kPresentationController = 1 << 13,
  kTopNavigationByUserActivation = 1 << 14,
  kDownloads = 1 << 15,
  kStorageAccessByUserActivation = 1 << 16,
  kTopNavigationToCustomProtocols = 1 << 17,
  kAllowSameSiteNoneCookies = 1 << 18,
  kAll = -1,
};

constexpr SandboxFlags operator|(SandboxFlags a, SandboxFlags b) {
  return static_cast<SandboxFlags>(static_cast<int32_t>(a) |
                                   static_cast<int32_t>(b));
}

constexpr SandboxFlags operator&(SandboxFlags a, SandboxFlags b) {
  return static_cast<SandboxFlags>(static_cast<int32_t>(a) &
                                   static_cast<int32_t>(b));
}

constexpr SandboxFlags operator~(SandboxFlags flags) {
  return static_cast<SandboxFlags>(~static_cast<int32_t>(flags));
}

struct SandboxPolicy {
  // What the attribute withholds: all, but what its tokens allow.
  SandboxFlags flags = SandboxFlags::kAll;
  // What to say of the tokens that allow nothing, or nothing.
  std::string error_message;
};

// ParseWebSandboxPolicy() of an attribute's value in UTF-8.
SandboxPolicy ParseSandboxPolicy(std::string_view input);

}  // namespace mitto

#endif  // MITTO_DOM_SANDBOX_FLAGS_H_
