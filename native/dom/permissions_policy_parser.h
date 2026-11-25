// PermissionsPolicyParser: the parser of an iframe's allow attribute, which
// reads Feature Policy's syntax, and of what the library has read of a
// response's headers.
//
// Upstream: third_party/blink/renderer/core/permissions_policy/permissions_policy_parser.cc

#ifndef MITTO_DOM_PERMISSIONS_POLICY_PARSER_H_
#define MITTO_DOM_PERMISSIONS_POLICY_PARSER_H_

#include <string>
#include <string_view>
#include <vector>

#include "mitto/dom/permissions_policy_state.h"
#include "mitto/policy_allowlist.h"
#include "url/origin.h"

namespace mitto {

// The policy that `allow` of an iframe in a document of `self` says
// for a document of `source`, the origin of the iframe's URL.
ParsedPolicy ParseAllowAttribute(std::u16string_view allow,
                                 const url::Origin& self,
                                 const url::Origin& source,
                                 const bindings::RuntimeFeatureSet& features,
                                 std::vector<PolicyMessage>& messages);

// ParseHeader(): the policy that a response's headers say for its document, of
// `self`.
ParsedPolicy ParsePolicyHeaders(const PermissionsPolicyHeaders& headers,
                                const url::Origin& self,
                                const bindings::RuntimeFeatureSet& features,
                                std::vector<PolicyMessage>& messages);

// AllowFeatureEverywhereIfNotPresent(): whether `policy` said nothing of it.
bool AllowEverywhereIfNotPresent(std::string_view feature,
                                 ParsedPolicy& policy);

// DisallowFeatureIfNotPresent().
void DisallowIfNotPresent(std::string_view feature, ParsedPolicy& policy);

}  // namespace mitto

#endif  // MITTO_DOM_PERMISSIONS_POLICY_PARSER_H_
