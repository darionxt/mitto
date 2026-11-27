// A response's Permissions-Policy and Feature-Policy headers, which Blink
// parses in the renderer and the library parses here, as it has //net's parser
// of structured headers and cross-origin isolation asks them.
//
// Upstream: third_party/blink/renderer/core/permissions_policy/permissions_policy_parser.cc

#ifndef MITTO_PERMISSIONS_POLICY_HEADER_H_
#define MITTO_PERMISSIONS_POLICY_HEADER_H_

#include <optional>
#include <string>

#include "mitto/policy_allowlist.h"

namespace mitto {

// Of the values of the headers, the lines of each joined.
PermissionsPolicyHeaders ParsePermissionsPolicyHeaders(
    const std::optional<std::string>& feature_policy,
    const std::optional<std::string>& permissions_policy);

}  // namespace mitto

#endif  // MITTO_PERMISSIONS_POLICY_HEADER_H_
