#include "mitto/dom/wgsl_language_features.h"

#include "mitto/bindings/v8_wgsl_language_features.h"

namespace mitto {
namespace {

// What GatherWGSLLanguageFeatures() gives without --enable-unsafe-webgpu and
// WebGPUExperimentalFeatures, which the stable set lacks, in the order Chrome
// 157 gives them (Mac Chrome 157.0.8103.51, 2026-11-13).
constexpr const char16_t* kFeatures[] = {
    u"packed_4x8_integer_dot_product",
    u"subgroup_uniformity",
    u"immediate_address_space",
    u"subgroup_id",
    u"linear_indexing",
    u"readonly_and_readwrite_storage_textures",
    u"swizzle_assignment",
    u"unrestricted_pointer_parameters",
    u"buffer_view",
    u"texture_formats_tier1",
    u"pointer_composite_access",
    u"texture_and_sampler_let",
    u"uniform_buffer_standard_layout",
};

}  // namespace

// static
WGSLLanguageFeatures* WGSLLanguageFeatures::Create() {
  return bindings::MakeGarbageCollected<WGSLLanguageFeatures>();
}

// static
bindings::ScriptWrappable* WGSLLanguageFeatures::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<WGSLLanguageFeatures>(
      info, bindings::V8WGSLLanguageFeatures::kWrapperTypeInfo);
}

WGSLLanguageFeatures::WGSLLanguageFeatures() = default;
WGSLLanguageFeatures::~WGSLLanguageFeatures() = default;

const bindings::WrapperTypeInfo* WGSLLanguageFeatures::GetWrapperTypeInfo()
    const {
  return &bindings::V8WGSLLanguageFeatures::kWrapperTypeInfo;
}

std::vector<std::u16string> WGSLLanguageFeatures::SetlikeValues() const {
  return std::vector<std::u16string>(std::begin(kFeatures),
                                     std::end(kFeatures));
}

}  // namespace mitto
