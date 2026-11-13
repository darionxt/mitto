#include "mitto/dom/permissions_policy.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>

#include "base/hash/hash.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/v8_permissions_policy.h"
#include "mitto/string_hasher.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "url/scheme_host_port.h"
#include "url/url_constants.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

namespace {

using bindings::RuntimeFeature;

// A feature's default allowlist, as PermissionsPolicyFeatureDefault has it.
enum class Default { kEnableForAll, kEnableForSelf };

// What a feature of the name map waits on, as the generated
// GetDefaultFeatureNameMap() has it: nothing, a runtime-enabled feature, which
// it is in the map only with, or an origin trial, which leaves it in the map
// but out of what a page sees while its runtime-enabled feature is off.
enum class Gate { kNone, kRuntimeFeature, kOriginTrial };

}  // namespace

struct PolicyFeature {
  std::string_view name;
  Default default_allowlist;
  Gate gate;
  std::optional<RuntimeFeature> feature;
};

namespace {

// permissions_policy_features.json5 of the Engine Release has 104 features,
// which the name map reserves room for.
constexpr size_t kFeatureCount = 104;

// The features that a window's name map can hold, in the order that
// GetDefaultFeatureNameMap() inserts them: those that wait on nothing or on an
// origin trial in the order of the json5, and then those that wait on a
// runtime-enabled feature, by the name of that feature. The features that only
// isolated contexts or nothing at all can see are left out.
constexpr PolicyFeature kPolicyFeatures[] = {
    {"accelerometer", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"autoplay", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"camera", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"ch-dpr", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"ch-device-memory", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"ch-downlink", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"ch-ect", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"ch-prefers-color-scheme", Default::kEnableForSelf, Gate::kNone,
     std::nullopt},
    {"ch-prefers-reduced-motion", Default::kEnableForSelf, Gate::kNone,
     std::nullopt},
    {"ch-prefers-reduced-transparency", Default::kEnableForSelf, Gate::kNone,
     std::nullopt},
    {"ch-rtt", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"ch-save-data", Default::kEnableForAll, Gate::kNone, std::nullopt},
    {"ch-ua", Default::kEnableForAll, Gate::kNone, std::nullopt},
    {"ch-ua-arch", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"ch-ua-bitness", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"ch-ua-form-factors", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"ch-ua-high-entropy-values", Default::kEnableForAll, Gate::kNone,
     std::nullopt},
    {"ch-ua-platform", Default::kEnableForAll, Gate::kNone, std::nullopt},
    {"ch-ua-model", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"ch-ua-mobile", Default::kEnableForAll, Gate::kNone, std::nullopt},
    {"ch-ua-full-version", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"ch-ua-full-version-list", Default::kEnableForSelf, Gate::kNone,
     std::nullopt},
    {"ch-ua-platform-version", Default::kEnableForSelf, Gate::kNone,
     std::nullopt},
    {"ch-ua-wow64", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"ch-viewport-width", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"ch-width", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"clipboard-read", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"clipboard-write", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"cross-origin-isolated", Default::kEnableForSelf, Gate::kNone,
     std::nullopt},
    {"display-capture", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"encrypted-media", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"focus-without-user-activation", Default::kEnableForAll,
     Gate::kOriginTrial, RuntimeFeature::kBlockingFocusWithoutUserActivation},
    {"fullscreen", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"frobulate", Default::kEnableForSelf, Gate::kOriginTrial,
     RuntimeFeature::kOriginTrialsSampleAPI},
    {"gamepad", Default::kEnableForAll, Gate::kNone, std::nullopt},
    {"geolocation", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"gyroscope", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"identity-credentials-get", Default::kEnableForSelf, Gate::kNone,
     std::nullopt},
    {"idle-detection", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"keyboard-map", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"local-network", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"loopback-network", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"magnetometer", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"microphone", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"midi", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"payment", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"picture-in-picture", Default::kEnableForAll, Gate::kNone, std::nullopt},
    {"publickey-credentials-create", Default::kEnableForSelf, Gate::kNone,
     std::nullopt},
    {"publickey-credentials-get", Default::kEnableForSelf, Gate::kNone,
     std::nullopt},
    {"rewriter", Default::kEnableForSelf, Gate::kOriginTrial,
     RuntimeFeature::kAIRewriterAPI},
    {"speaker-selection", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"storage-access", Default::kEnableForAll, Gate::kNone, std::nullopt},
    {"sync-xhr", Default::kEnableForAll, Gate::kNone, std::nullopt},
    {"unload", Default::kEnableForAll, Gate::kNone, std::nullopt},
    {"web-app-installation", Default::kEnableForSelf, Gate::kOriginTrial,
     RuntimeFeature::kWebAppInstallation},
    {"tools", Default::kEnableForSelf, Gate::kOriginTrial,
     RuntimeFeature::kWebMCP},
    {"window-management", Default::kEnableForSelf, Gate::kNone, std::nullopt},
    {"writer", Default::kEnableForSelf, Gate::kOriginTrial,
     RuntimeFeature::kAIWriterAPI},
    {"language-model", Default::kEnableForSelf, Gate::kRuntimeFeature,
     RuntimeFeature::kAIPromptAPI},
    {"summarizer", Default::kEnableForSelf, Gate::kRuntimeFeature,
     RuntimeFeature::kAISummarizationAPI},
    {"aria-notify", Default::kEnableForAll, Gate::kRuntimeFeature,
     RuntimeFeature::kAriaNotify},
    {"autofill", Default::kEnableForSelf, Gate::kRuntimeFeature,
     RuntimeFeature::kAutofill},
    {"captured-surface-control", Default::kEnableForSelf, Gate::kRuntimeFeature,
     RuntimeFeature::kCapturedSurfaceControl},
    {"compute-pressure", Default::kEnableForSelf, Gate::kRuntimeFeature,
     RuntimeFeature::kComputePressure},
    {"device-attributes", Default::kEnableForSelf, Gate::kRuntimeFeature,
     RuntimeFeature::kDeviceAttributes},
    {"vertical-scroll", Default::kEnableForAll, Gate::kRuntimeFeature,
     RuntimeFeature::kExperimentalPolicies},
    {"deferred-fetch", Default::kEnableForSelf, Gate::kRuntimeFeature,
     RuntimeFeature::kFetchLaterAPI},
    {"deferred-fetch-minimal", Default::kEnableForAll, Gate::kRuntimeFeature,
     RuntimeFeature::kFetchLaterAPI},
    {"local-fonts", Default::kEnableForSelf, Gate::kRuntimeFeature,
     RuntimeFeature::kFontAccess},
    {"execution-while-out-of-viewport", Default::kEnableForAll,
     Gate::kRuntimeFeature, RuntimeFeature::kFreezeFramesOnVisibility},
    {"execution-while-not-rendered", Default::kEnableForAll,
     Gate::kRuntimeFeature, RuntimeFeature::kFreezeFramesOnVisibility},
    {"language-detector", Default::kEnableForSelf, Gate::kRuntimeFeature,
     RuntimeFeature::kLanguageDetectionAPI},
    {"local-network-access", Default::kEnableForSelf, Gate::kRuntimeFeature,
     RuntimeFeature::kLocalNetworkAccessPermissionPolicy},
    {"webnn", Default::kEnableForSelf, Gate::kRuntimeFeature,
     RuntimeFeature::kMachineLearningNeuralNetwork},
    {"manual-text", Default::kEnableForSelf, Gate::kRuntimeFeature,
     RuntimeFeature::kManualText},
    {"media-playback-while-not-visible", Default::kEnableForAll,
     Gate::kRuntimeFeature,
     RuntimeFeature::kMediaPlaybackWhileNotVisiblePermissionPolicy},
    {"on-device-speech-recognition", Default::kEnableForSelf,
     Gate::kRuntimeFeature, RuntimeFeature::kOnDeviceWebSpeechAvailable},
    {"private-state-token-issuance", Default::kEnableForAll,
     Gate::kRuntimeFeature, RuntimeFeature::kPrivateStateTokens},
    {"private-state-token-redemption", Default::kEnableForAll,
     Gate::kRuntimeFeature, RuntimeFeature::kPrivateStateTokens},
    {"ambient-light-sensor", Default::kEnableForSelf, Gate::kRuntimeFeature,
     RuntimeFeature::kSensorExtraClasses},
    {"serial", Default::kEnableForSelf, Gate::kRuntimeFeature,
     RuntimeFeature::kSerial},
    {"browsing-topics", Default::kEnableForAll, Gate::kRuntimeFeature,
     RuntimeFeature::kTopicsAPI},
    {"interest-cohort", Default::kEnableForAll, Gate::kRuntimeFeature,
     RuntimeFeature::kTopicsAPI},
    {"translator", Default::kEnableForSelf, Gate::kRuntimeFeature,
     RuntimeFeature::kTranslationAPI},
    {"ch-viewport-height", Default::kEnableForSelf, Gate::kRuntimeFeature,
     RuntimeFeature::kViewportHeightClientHintHeader},
    {"screen-wake-lock", Default::kEnableForSelf, Gate::kRuntimeFeature,
     RuntimeFeature::kWakeLock},
    {"publickey-credentials-remote-client-data-json", Default::kEnableForSelf,
     Gate::kRuntimeFeature,
     RuntimeFeature::kWebAuthenticationRemoteClientDataJson},
    {"bluetooth", Default::kEnableForSelf, Gate::kRuntimeFeature,
     RuntimeFeature::kWebBluetooth},
    {"hid", Default::kEnableForSelf, Gate::kRuntimeFeature,
     RuntimeFeature::kWebHID},
    {"haptics", Default::kEnableForSelf, Gate::kRuntimeFeature,
     RuntimeFeature::kWebHaptics},
    {"digital-credentials-get", Default::kEnableForSelf, Gate::kRuntimeFeature,
     RuntimeFeature::kWebIdentityDigitalCredentials},
    {"digital-credentials-create", Default::kEnableForSelf,
     Gate::kRuntimeFeature,
     RuntimeFeature::kWebIdentityDigitalCredentialsCreation},
    {"otp-credentials", Default::kEnableForSelf, Gate::kRuntimeFeature,
     RuntimeFeature::kWebOTPAssertionFeaturePolicy},
    {"web-share", Default::kEnableForSelf, Gate::kRuntimeFeature,
     RuntimeFeature::kWebShare},
    {"usb", Default::kEnableForSelf, Gate::kRuntimeFeature,
     RuntimeFeature::kWebUSB},
    {"xr-spatial-tracking", Default::kEnableForSelf, Gate::kRuntimeFeature,
     RuntimeFeature::kWebXR},
};

// The only feature whose state is measured but never shown,
// IsFeatureForMeasurementOnly().
constexpr std::string_view kMeasurementOnly = "web-share";
constexpr std::string_view kUnload = "unload";

// WTF's HashTable::CalculateCapacity(): the smallest power of two above
// `size`, doubled.
constexpr size_t CalculateCapacity(size_t size) {
  for (size_t mask = size; mask; mask >>= 1) {
    size |= mask;
  }
  return (size + 1) * 2;
}

// The features of the window's name map in the order that the map iterates
// them: its buckets, each found by the name's hash and triangular probing.
std::vector<const PolicyFeature*> FeaturesInMapOrder(
    const bindings::RuntimeFeatureSet& features) {
  constexpr size_t kCapacity = CalculateCapacity(kFeatureCount);
  std::array<const PolicyFeature*, kCapacity> buckets{};
  for (const PolicyFeature& feature : kPolicyFeatures) {
    if (feature.gate == Gate::kRuntimeFeature &&
        !features.IsEnabled(*feature.feature)) {
      continue;
    }
    size_t index = HashString24(feature.name) & (kCapacity - 1);
    for (size_t probe = 1; buckets[index]; ++probe) {
      index = (index + probe) & (kCapacity - 1);
    }
    buckets[index] = &feature;
  }
  std::vector<const PolicyFeature*> ordered;
  for (const PolicyFeature* feature : buckets) {
    if (feature) {
      ordered.push_back(feature);
    }
  }
  return ordered;
}

}  // namespace

bool IsUnloadDeprecated(const DeprecatedUnload& seed, const GURL& url) {
  if (!seed.enabled) {
    return false;
  }
  // UnloadDeprecationAllowedForOrigin(): every host, without an allow list
  // and buckets.
  if (!seed.allow_list && !seed.by_bucket) {
    return true;
  }
  // Only on http(s) pages, with an opaque origin's precursor for it. The
  // tuple is the origin's, which has to outlive it.
  const url::Origin origin = url::Origin::Create(url);
  const url::SchemeHostPort& tuple = origin.GetTupleOrPrecursorTupleIfOpaque();
  if (tuple.scheme() != url::kHttpScheme &&
      tuple.scheme() != url::kHttpsScheme) {
    return false;
  }
  // The hosts on the allow list, or every host for an empty one.
  if (seed.allow_list &&
      (seed.allow_list->empty() ||
       std::ranges::contains(*seed.allow_list, tuple.host()))) {
    return true;
  }
  if (!seed.by_bucket) {
    return false;
  }
  // IsIncludedInGradualRollout(): the host's hash, hashed with the bucket.
  const int percent = seed.by_bucket->percent;
  if (percent == 100) {
    return true;
  }
  if (percent == 0) {
    return false;
  }
  // Without a bucket, as where the seed gives each installation its own, the
  // Persona is in none.
  if (!seed.by_bucket->bucket) {
    return false;
  }
  const std::array<uint8_t, 2> hash = {
      static_cast<uint8_t>(base::PersistentHash(tuple.host())),
      static_cast<uint8_t>(*seed.by_bucket->bucket)};
  return static_cast<int>(base::PersistentHash(hash) % 100) < percent;
}

// static
PermissionsPolicy* PermissionsPolicy::Create(const std::u16string& url,
                                             bool unload_deprecated) {
  return bindings::MakeGarbageCollected<PermissionsPolicy>(url,
                                                           unload_deprecated);
}

PermissionsPolicy::PermissionsPolicy(const std::u16string& url,
                                     bool unload_deprecated)
    : origin_(url::Origin::Create(GURL(base::UTF16ToUTF8(url)))),
      unload_deprecated_(unload_deprecated) {
  const bindings::RuntimeFeatureSet& features =
      bindings::PerIsolateData::From(v8::Isolate::GetCurrent())->features();
  // FeatureAvailable(): a feature of the map that no origin trial hides and
  // whose state is not only measured.
  for (const PolicyFeature* feature : FeaturesInMapOrder(features)) {
    if (feature->gate == Gate::kOriginTrial &&
        !features.IsEnabled(*feature->feature)) {
      continue;
    }
    if (feature->name == kMeasurementOnly) {
      continue;
    }
    available_.push_back(feature);
  }
}

PermissionsPolicy::~PermissionsPolicy() = default;

const bindings::WrapperTypeInfo* PermissionsPolicy::GetWrapperTypeInfo() const {
  return &bindings::V8PermissionsPolicy::kWrapperTypeInfo;
}

bool PermissionsPolicy::AllowsFeature(
    const std::u16string& feature,
    const std::optional<std::u16string>& origin) const {
  if (!origin) {
    const PolicyFeature* const found = Find(feature);
    return found && IsEnabledForOrigin(*found, origin_);
  }
  // SecurityOrigin::CreateFromString(): a URL that does not parse has an
  // opaque origin. Blink warns on the console of either refusal, which a Page
  // does not keep.
  const url::Origin other =
      url::Origin::Create(GURL(base::UTF16ToUTF8(*origin)));
  if (other.opaque()) {
    return false;
  }
  const PolicyFeature* const found = Find(feature);
  return found && IsEnabledForOrigin(*found, other);
}

std::vector<std::u16string> PermissionsPolicy::Features() const {
  std::vector<std::u16string> names;
  for (const PolicyFeature* feature : available_) {
    names.push_back(base::ASCIIToUTF16(feature->name));
  }
  return names;
}

std::vector<std::u16string> PermissionsPolicy::AllowedFeatures() const {
  std::vector<std::u16string> names;
  for (const PolicyFeature* feature : available_) {
    if (IsEnabledForOrigin(*feature, origin_)) {
      names.push_back(base::ASCIIToUTF16(feature->name));
    }
  }
  return names;
}

std::vector<std::u16string> PermissionsPolicy::GetAllowlistForFeature(
    const std::u16string& feature) const {
  const PolicyFeature* const found = Find(feature);
  if (!found || IsDisabled(*found)) {
    return {};
  }
  if (found->default_allowlist == Default::kEnableForAll) {
    return {u"*"};
  }
  // OriginWithPossibleWildcards::FromOrigin() has none of an opaque origin.
  if (origin_.opaque()) {
    return {};
  }
  return {base::UTF8ToUTF16(origin_.Serialize())};
}

const PolicyFeature* PermissionsPolicy::Find(const std::u16string& name) const {
  for (const PolicyFeature* feature : available_) {
    if (base::ASCIIToUTF16(feature->name) == name) {
      return feature;
    }
  }
  return nullptr;
}

bool PermissionsPolicy::IsDisabled(const PolicyFeature& feature) const {
  // GetPermissionsPolicyFeatureList(): the seed's kDeprecateUnload gives
  // unload no origin at all.
  return unload_deprecated_ && feature.name == kUnload;
}

bool PermissionsPolicy::IsEnabledForOrigin(const PolicyFeature& feature,
                                           const url::Origin& origin) const {
  // A top-level document with no Permissions-Policy header inherits every
  // feature and declares none, so the default allowlist decides.
  if (IsDisabled(feature)) {
    return false;
  }
  return feature.default_allowlist == Default::kEnableForAll ||
         origin_.IsSameOriginWith(origin);
}

}  // namespace mitto
