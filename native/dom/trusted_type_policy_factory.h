// Trusted Types' TrustedTypePolicyFactory, the window's trustedTypes, with no
// default policy, as a page has until it makes one: Blink's table of the
// attributes that take a trusted type, and whether a value is an object of
// each trusted type. Making policies and their objects are Known Gaps.
//
// Upstream: third_party/blink/renderer/core/trustedtypes/trusted_type_policy_factory.cc
// Upstream: third_party/blink/renderer/core/trustedtypes/generate_eventhandler_names.py
// Upstream: third_party/blink/renderer/core/events/event_type_names.json5
// Upstream: third_party/blink/renderer/core/html/html_attribute_names.json5
// Upstream: third_party/blink/renderer/core/svg/svg_attribute_names.json5
// Upstream: third_party/blink/renderer/core/mathml/mathml_attribute_names.json5

#ifndef MITTO_DOM_TRUSTED_TYPE_POLICY_FACTORY_H_
#define MITTO_DOM_TRUSTED_TYPE_POLICY_FACTORY_H_

#include <optional>
#include <string>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-value.h"

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
  bool IsHTML(v8::Local<v8::Value> checked_object) const;
  bool IsScript(v8::Local<v8::Value> checked_object) const;
  bool IsScriptURL(v8::Local<v8::Value> checked_object) const;
  std::optional<std::u16string> GetAttributeType(
      const std::u16string& tag_name,
      const std::u16string& attribute,
      const std::optional<std::u16string>& element_ns,
      const std::optional<std::u16string>& attr_ns) const;
};

}  // namespace mitto

#endif  // MITTO_DOM_TRUSTED_TYPE_POLICY_FACTORY_H_
