// WebGPU's WGSLLanguageFeatures, navigator.gpu.wgslLanguageFeatures: the WGSL
// language features that Chrome's Dawn wire client gives a page, which follow
// the release and not the machine, in the order Blink's HashSet of them
// iterates, as Chrome 157 gives it.
//
// Upstream: third_party/blink/renderer/modules/webgpu/wgsl_language_features.cc
// Upstream: third_party/blink/renderer/platform/graphics/gpu/dawn_control_client_holder.cc

#ifndef MITTO_DOM_WGSL_LANGUAGE_FEATURES_H_
#define MITTO_DOM_WGSL_LANGUAGE_FEATURES_H_

#include <string>
#include <vector>

#include "mitto/bindings/script_wrappable.h"

namespace mitto {

class WGSLLanguageFeatures final : public bindings::ScriptWrappable {
 public:
  static WGSLLanguageFeatures* Create();
  // The one a Known Gap gives, whose wrapper is of `info`.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  WGSLLanguageFeatures();
  ~WGSLLanguageFeatures() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The setlike's values: the features' names.
  std::vector<std::u16string> SetlikeValues() const;
};

}  // namespace mitto

#endif  // MITTO_DOM_WGSL_LANGUAGE_FEATURES_H_
