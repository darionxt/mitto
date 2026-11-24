// Permissions Policy's PermissionsPolicy, document.featurePolicy and
// iframe.featurePolicy: the policy-controlled features of the Page's
// runtime-enabled features, in the order of Blink's hash map of their names,
// each allowed as its document's permissions policy allows it, but "unload",
// which the Persona's variations seed may deprecate for the document's host.
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

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/permissions_policy_state.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class Document;

class PermissionsPolicy final : public bindings::ScriptWrappable {
 public:
  // document.featurePolicy, DocumentPolicy.
  static PermissionsPolicy* Create(Document& document);
  // iframe.featurePolicy of an iframe in `document`, IFramePolicy, which says
  // what a document at its URL would have.
  static PermissionsPolicy* Create(
      Document& document,
      std::unique_ptr<PermissionsPolicyState> state);

  PermissionsPolicy(Document& document,
                    std::unique_ptr<PermissionsPolicyState> state);
  ~PermissionsPolicy() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // IFramePolicy::UpdateContainerPolicy().
  void SetFrameState(std::unique_ptr<PermissionsPolicyState> state) {
    frame_state_ = std::move(state);
  }

  // The members of the interface.
  bool AllowsFeature(const std::u16string& feature,
                     const std::optional<std::u16string>& origin);
  std::vector<std::u16string> Features() const;
  std::vector<std::u16string> AllowedFeatures() const;
  std::vector<std::u16string> GetAllowlistForFeature(
      const std::u16string& feature);

 private:
  const PermissionsPolicyState& State() const;
  // The feature that a page names, if a page can see it; the console says if
  // not.
  const PolicyFeature* FindOrWarn(const std::u16string& name);
  void Warn(std::string message);

  const cppgc::Member<Document> document_;
  // An iframe's.
  std::unique_ptr<PermissionsPolicyState> frame_state_;
  // What a page sees of the window's name map, in its order.
  std::vector<const PolicyFeature*> available_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PERMISSIONS_POLICY_H_
