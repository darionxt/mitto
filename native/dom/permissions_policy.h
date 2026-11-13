// Permissions Policy's PermissionsPolicy, document.featurePolicy, for a
// top-level document whose response has no Permissions-Policy header: the
// policy-controlled features of the Page's runtime-enabled features, in the
// order of Blink's hash map of their names, each allowed by its default
// allowlist, but "unload", which the Persona's variations seed may deprecate
// for the document's host.
//
// Upstream: third_party/blink/renderer/core/permissions_policy/dom_feature_policy.cc
// Upstream: third_party/blink/renderer/core/permissions_policy/permissions_policy_parser.cc
// Upstream: third_party/blink/renderer/build/scripts/templates/policy_helper.cc.tmpl
// Upstream: third_party/blink/renderer/core/dom/document.cc
// Upstream: third_party/blink/renderer/platform/wtf/hash_table.h
// Upstream: services/network/public/cpp/permissions_policy/permissions_policy.cc
// Upstream: services/network/public/cpp/permissions_policy/permissions_policy_features.cc
// Upstream: services/network/public/cpp/permissions_policy/permissions_policy_features_internal.cc
// Upstream: services/network/public/cpp/permissions_policy/permissions_policy_features.json5

#ifndef MITTO_DOM_PERMISSIONS_POLICY_H_
#define MITTO_DOM_PERMISSIONS_POLICY_H_

#include <optional>
#include <string>
#include <vector>

#include "mitto/bindings/script_wrappable.h"
#include "mitto/page_messages.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace mitto {

struct PolicyFeature;

// Whether the seed's kDeprecateUnload takes unload away from a document at
// `url`, as Chrome's network service decides it for the document's origin.
bool IsUnloadDeprecated(const DeprecatedUnload& seed, const GURL& url);

class PermissionsPolicy final : public bindings::ScriptWrappable {
 public:
  // Of a document at `url`, which `unload_deprecated` says whether the seed
  // deprecates unload for.
  static PermissionsPolicy* Create(const std::u16string& url,
                                   bool unload_deprecated);

  PermissionsPolicy(const std::u16string& url, bool unload_deprecated);
  ~PermissionsPolicy() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  bool AllowsFeature(const std::u16string& feature,
                     const std::optional<std::u16string>& origin) const;
  std::vector<std::u16string> Features() const;
  std::vector<std::u16string> AllowedFeatures() const;
  std::vector<std::u16string> GetAllowlistForFeature(
      const std::u16string& feature) const;

 private:
  // The feature that a page names, if a page can see it.
  const PolicyFeature* Find(const std::u16string& name) const;
  // Whether no origin has the feature, as unload once deprecated.
  bool IsDisabled(const PolicyFeature& feature) const;
  bool IsEnabledForOrigin(const PolicyFeature& feature,
                          const url::Origin& origin) const;

  const url::Origin origin_;
  const bool unload_deprecated_;
  // What a page sees of the window's name map, in its order.
  std::vector<const PolicyFeature*> available_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PERMISSIONS_POLICY_H_
