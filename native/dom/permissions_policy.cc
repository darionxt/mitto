#include "mitto/dom/permissions_policy.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "base/hash/hash.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/v8_permissions_policy.h"
#include "mitto/dom/document.h"
#include "mitto/dom/frame.h"
#include "mitto/dom/permissions_policy_state.h"
#include "mitto/dom/string_hash_table_order.h"
#include "mitto/dom/window.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "url/scheme_host_port.h"
#include "url/url_constants.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

namespace {

using bindings::RuntimeFeature;
using Default = PolicyFeature::Default;
using Gate = PolicyFeature::Gate;

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

// The features of the window's name map in the order that the map iterates
// them, which ReserveCapacityForSize() sized for every feature of the json5.
std::vector<const PolicyFeature*> FeaturesInMapOrder(
    const bindings::RuntimeFeatureSet& features) {
  StringHashTableOrder table =
      StringHashTableOrder::WithCapacityFor(kFeatureCount);
  std::map<std::string_view, const PolicyFeature*> by_name;
  for (const PolicyFeature& feature : kPolicyFeatures) {
    if (feature.gate == Gate::kRuntimeFeature &&
        !features.IsEnabled(*feature.feature)) {
      continue;
    }
    table.Insert(feature.name);
    by_name.emplace(feature.name, &feature);
  }
  std::vector<const PolicyFeature*> ordered;
  for (const std::string& name : table.Keys()) {
    ordered.push_back(by_name.at(name));
  }
  return ordered;
}

}  // namespace

base::span<const PolicyFeature> PolicyFeatures() {
  return kPolicyFeatures;
}

const PolicyFeature& PolicyFeatureNamed(std::string_view name) {
  return *std::ranges::find(kPolicyFeatures, name, &PolicyFeature::name);
}

bool IsUnloadDeprecated(const DeprecatedUnload& seed,
                        const url::Origin& origin) {
  if (!seed.enabled) {
    return false;
  }
  // UnloadDeprecationAllowedForOrigin(): every host, without an allow list
  // and buckets.
  if (!seed.allow_list && !seed.by_bucket) {
    return true;
  }
  // Only on http(s) pages, with an opaque origin's precursor for it.
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
PermissionsPolicy* PermissionsPolicy::Create(Window& window) {
  return bindings::MakeGarbageCollected<PermissionsPolicy>(window, nullptr);
}

// static
PermissionsPolicy* PermissionsPolicy::Create(
    Window& window,
    std::unique_ptr<PermissionsPolicyState> state) {
  return bindings::MakeGarbageCollected<PermissionsPolicy>(window,
                                                           std::move(state));
}

PermissionsPolicy::PermissionsPolicy(
    Window& window,
    std::unique_ptr<PermissionsPolicyState> state)
    : window_(window), frame_state_(std::move(state)) {
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

void PermissionsPolicy::Trace(cppgc::Visitor* visitor) const {
  ScriptWrappable::Trace(visitor);
  visitor->Trace(window_);
}

bool PermissionsPolicy::AllowsFeature(
    const std::u16string& feature,
    const std::optional<std::u16string>& origin) {
  if (!origin) {
    const PolicyFeature* const found = FindOrWarn(feature);
    return found && State().IsEnabled(*found);
  }
  // SecurityOrigin::CreateFromString(): a URL that does not parse has an
  // opaque origin.
  const url::Origin other =
      url::Origin::Create(GURL(base::UTF16ToUTF8(*origin)));
  if (other.opaque()) {
    Warn(base::StrCat({"Invalid origin url for feature '",
                       base::UTF16ToUTF8(feature),
                       "': ", base::UTF16ToUTF8(*origin), "."}));
    return false;
  }
  const PolicyFeature* const found = FindOrWarn(feature);
  return found && State().IsEnabledFor(*found, other);
}

std::vector<std::u16string> PermissionsPolicy::Features() const {
  std::vector<std::u16string> names;
  for (const PolicyFeature* feature : available_) {
    names.push_back(base::ASCIIToUTF16(feature->name));
  }
  return names;
}

std::vector<std::u16string> PermissionsPolicy::AllowedFeatures() const {
  const PermissionsPolicyState& state = State();
  std::vector<std::u16string> names;
  for (const PolicyFeature* feature : available_) {
    if (state.IsEnabled(*feature)) {
      names.push_back(base::ASCIIToUTF16(feature->name));
    }
  }
  return names;
}

std::vector<std::u16string> PermissionsPolicy::GetAllowlistForFeature(
    const std::u16string& feature) {
  const PolicyFeature* const found = FindOrWarn(feature);
  return found ? State().AllowlistFor(*found) : std::vector<std::u16string>();
}

const PermissionsPolicyState& PermissionsPolicy::State() const {
  return frame_state_ ? *frame_state_
                      : window_->GetDocument().GetPermissionsPolicyState();
}

const PolicyFeature* PermissionsPolicy::FindOrWarn(const std::u16string& name) {
  for (const PolicyFeature* feature : available_) {
    if (base::ASCIIToUTF16(feature->name) == name) {
      return feature;
    }
  }
  Warn(
      base::StrCat({"Unrecognized feature: '", base::UTF16ToUTF8(name), "'."}));
  return nullptr;
}

void PermissionsPolicy::Warn(std::string message) {
  // One that an allow attribute does not say again.
  if (mitto::Frame* const frame = window_->GetFrame()) {
    frame->Top().AddOneTimeMessage(message);
  }
  window_->AddConsoleMessage(ConsoleLevel::kWarning, std::move(message));
}

}  // namespace mitto
