// Trusted Types' TrustedTypePolicyFactory, the window's trustedTypes, with no
// default policy, as a page has until it makes one. Its other members are
// Known Gaps.
//
// Upstream: third_party/blink/renderer/core/trustedtypes/trusted_type_policy_factory.cc

#ifndef MITTO_DOM_TRUSTED_TYPE_POLICY_FACTORY_H_
#define MITTO_DOM_TRUSTED_TYPE_POLICY_FACTORY_H_

#include "mitto/bindings/script_wrappable.h"

namespace mitto {

class TrustedTypePolicyFactory final : public bindings::ScriptWrappable {
 public:
  static TrustedTypePolicyFactory* Create();
  // The one a Known Gap gives, whose wrapper is of `info`.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  TrustedTypePolicyFactory();
  ~TrustedTypePolicyFactory() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  // Null, which Blink's gives until a page makes a policy named "default",
  // though the IDL does not make it nullable.
  bindings::ScriptWrappable* DefaultPolicy() const { return nullptr; }
};

}  // namespace mitto

#endif  // MITTO_DOM_TRUSTED_TYPE_POLICY_FACTORY_H_
