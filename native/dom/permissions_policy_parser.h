// The parser of an iframe's allow attribute, PermissionsPolicyParser's
// ParseAttribute(), which reads Feature Policy's syntax.
//
// Upstream: third_party/blink/renderer/core/permissions_policy/permissions_policy_parser.cc

#ifndef MITTO_DOM_PERMISSIONS_POLICY_PARSER_H_
#define MITTO_DOM_PERMISSIONS_POLICY_PARSER_H_

#include <string>
#include <string_view>
#include <vector>

#include "mitto/dom/permissions_policy_state.h"
#include "mitto/dom/window.h"
#include "url/origin.h"

namespace mitto {

// What the console is told of a policy, PolicyParserMessageBuffer.
struct PolicyMessage {
  ConsoleLevel level = ConsoleLevel::kWarning;
  std::string text;
};

// The policy that `allow` of an iframe in a document of `self` says
// for a document of `source`, the origin of the iframe's URL.
ContainerPolicy ParseAllowAttribute(std::u16string_view allow,
                                    const url::Origin& self,
                                    const url::Origin& source,
                                    const bindings::RuntimeFeatureSet& features,
                                    std::vector<PolicyMessage>& messages);

// AllowFeatureEverywhereIfNotPresent(): whether `policy` said nothing of it.
bool AllowEverywhereIfNotPresent(std::string_view feature,
                                 ContainerPolicy& policy);

// DisallowFeatureIfNotPresent().
void DisallowIfNotPresent(std::string_view feature, ContainerPolicy& policy);

}  // namespace mitto

#endif  // MITTO_DOM_PERMISSIONS_POLICY_PARSER_H_
