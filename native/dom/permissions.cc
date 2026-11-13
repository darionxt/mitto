#include "mitto/dom/permissions.h"

#include <array>
#include <optional>
#include <string_view>

#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_context.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_permissions.h"
#include "mitto/dom/permission_status.h"
#include "mitto/dom/script_values.h"
#include "mitto/dom/window.h"
#include "url/gurl.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

namespace {

using bindings::RuntimeFeature;

// The kinds of permission that Chrome's browser keeps, blink::PermissionType,
// of those that a descriptor can name.
enum class Kind {
  kGeolocation,
  kGeolocationApproximate,
  kNotifications,
  kMidi,
  kMidiSysex,
  kVideoCapture,
  kCameraPanTiltZoom,
  kAudioCapture,
  kBackgroundFetch,
  kBackgroundSync,
  kDurableStorage,
  kSensors,
  kWakeLockScreen,
  kWakeLockSystem,
  kNfc,
  kDisplayCapture,
  kClipboardReadWrite,
  kClipboardSanitizedWrite,
  kPaymentHandler,
  kIdleDetection,
  kPeriodicBackgroundSync,
  kStorageAccessGrant,
  kWindowManagement,
  kLocalFonts,
  kTopLevelStorageAccess,
  kCapturedSurfaceControl,
  kSpeakerSelection,
  kKeyboardLock,
  kPointerLock,
  kAutomaticFullscreen,
  kWebAppInstallation,
  kLocalNetworkAccess,
  kLocalNetwork,
  kLoopbackNetwork,
};

// What a descriptor asks about: the kind of permission, and the name that
// PermissionNameToString() gives its blink::mojom::PermissionName.
struct Descriptor {
  Kind kind;
  std::u16string_view name;
};

// The values of the IDL's enumeration PermissionName.
constexpr auto kPermissionNames = std::to_array<std::u16string_view>({
    u"geolocation",
    u"notifications",
    u"push",
    u"midi",
    u"camera",
    u"microphone",
    u"background-fetch",
    u"background-sync",
    u"persistent-storage",
    u"ambient-light-sensor",
    u"accelerometer",
    u"gyroscope",
    u"magnetometer",
    u"screen-wake-lock",
    u"nfc",
    u"display-capture",
    u"clipboard-read",
    u"clipboard-write",
    u"payment-handler",
    u"idle-detection",
    u"periodic-background-sync",
    u"system-wake-lock",
    u"storage-access",
    u"window-management",
    u"local-fonts",
    u"top-level-storage-access",
    u"captured-surface-control",
    u"speaker-selection",
    u"keyboard-lock",
    u"pointer-lock",
    u"fullscreen",
    u"web-app-installation",
    u"local-network-access",
    u"local-network",
    u"loopback-network",
    u"geolocation-approximate",
});

bool Enabled(v8::Isolate* isolate, RuntimeFeature feature) {
  return bindings::PerIsolateData::From(isolate)->features().IsEnabled(feature);
}

// A member of `object` as a dictionary reads it, undefined when absent.
// Returns false if its getter threw.
bool Read(v8::Isolate* isolate,
          v8::Local<v8::Object> object,
          bindings::DictionaryConversionScope& scope,
          const char* member,
          v8::Local<v8::Value>* value) {
  scope.SetCurrentPropertyName(member);
  return object
      ->Get(isolate->GetCurrentContext(),
            bindings::ToV8StringFromUtf8(isolate, member))
      .ToLocal(value);
}

// PermissionDescriptor's required member `name`, as NativeValueTraits of the
// descriptor `dictionary` reads it first, which its own members follow.
bool ReadName(v8::Isolate* isolate,
              v8::Local<v8::Object> object,
              bindings::ExceptionState& exception_state,
              std::u16string* name) {
  bindings::DictionaryConversionScope scope(isolate, "PermissionDescriptor");
  v8::Local<v8::Value> value;
  if (!Read(isolate, object, scope, "name", &value)) {
    return false;
  }
  if (value->IsUndefined()) {
    exception_state.ThrowTypeError("Required member is undefined.");
    return false;
  }
  return bindings::FromV8Enumeration(isolate, value, kPermissionNames,
                                     "PermissionName", name);
}

// A boolean member of a descriptor that inherits from PermissionDescriptor,
// false by default, read after its name as Blink's NativeValueTraits read it.
bool ReadBoolean(v8::Isolate* isolate,
                 v8::Local<v8::Object> object,
                 const char* dictionary,
                 const char* member,
                 bindings::ExceptionState& exception_state,
                 bool* result) {
  std::u16string name;
  if (!ReadName(isolate, object, exception_state, &name)) {
    return false;
  }
  bindings::DictionaryConversionScope scope(isolate, dictionary);
  v8::Local<v8::Value> value;
  if (!Read(isolate, object, scope, member, &value)) {
    return false;
  }
  *result = !value->IsUndefined() && value->BooleanValue(isolate);
  return true;
}

// Blink's ParsePermissionDescriptor(): the descriptor's kind, or nullopt if
// it threw.
std::optional<Descriptor> Parse(v8::Isolate* isolate,
                                v8::Local<v8::Object> object,
                                bindings::ExceptionState& exception_state) {
  std::u16string name;
  if (!ReadName(isolate, object, exception_state, &name)) {
    return std::nullopt;
  }
  const auto type_error = [&](std::string_view message) {
    exception_state.ThrowTypeError(message);
    return std::nullopt;
  };
  if (name == u"geolocation") {
    if (Enabled(isolate, RuntimeFeature::kApproximateGeolocationPermission) &&
        !Enabled(isolate,
                 RuntimeFeature::kApproximateGeolocationPermissionAPI)) {
      return Descriptor{Kind::kGeolocationApproximate, u"geolocation"};
    }
    return Descriptor{Kind::kGeolocation, u"geolocation"};
  }
  if (name == u"geolocation-approximate") {
    if (!Enabled(isolate,
                 RuntimeFeature::kApproximateGeolocationPermissionAPI)) {
      return type_error(
          "Permission API support for approximate geolocation is not "
          "enabled.");
    }
    return Descriptor{Kind::kGeolocationApproximate,
                      u"geolocation-approximate"};
  }
  if (name == u"notifications") {
    return Descriptor{Kind::kNotifications, u"notifications"};
  }
  if (name == u"push") {
    bool user_visible_only = false;
    if (!ReadBoolean(isolate, object, "PushPermissionDescriptor",
                     "userVisibleOnly", exception_state, &user_visible_only)) {
      return std::nullopt;
    }
    if (!user_visible_only) {
      exception_state.ThrowDOMException(
          "NotSupportedError",
          "Push Permission without userVisibleOnly:true isn't supported yet.");
      return std::nullopt;
    }
    return Descriptor{Kind::kNotifications, u"notifications"};
  }
  if (name == u"midi") {
    bool sysex = false;
    if (!ReadBoolean(isolate, object, "MidiPermissionDescriptor", "sysex",
                     exception_state, &sysex)) {
      return std::nullopt;
    }
    return Descriptor{sysex ? Kind::kMidiSysex : Kind::kMidi, u"midi"};
  }
  if (name == u"camera") {
    bool pan_tilt_zoom = false;
    if (!ReadBoolean(isolate, object, "CameraDevicePermissionDescriptor",
                     "panTiltZoom", exception_state, &pan_tilt_zoom)) {
      return std::nullopt;
    }
    return Descriptor{
        pan_tilt_zoom ? Kind::kCameraPanTiltZoom : Kind::kVideoCapture,
        u"video_capture"};
  }
  if (name == u"microphone") {
    return Descriptor{Kind::kAudioCapture, u"audio_capture"};
  }
  if (name == u"background-fetch") {
    return Descriptor{Kind::kBackgroundFetch, u"background_fetch"};
  }
  if (name == u"background-sync") {
    return Descriptor{Kind::kBackgroundSync, u"background_sync"};
  }
  if (name == u"persistent-storage") {
    return Descriptor{Kind::kDurableStorage, u"durable_storage"};
  }
  if (name == u"ambient-light-sensor" &&
      !Enabled(isolate, RuntimeFeature::kSensorExtraClasses)) {
    return type_error("GenericSensorExtraClasses flag is not enabled.");
  }
  if (name == u"ambient-light-sensor" || name == u"accelerometer" ||
      name == u"gyroscope" || name == u"magnetometer") {
    return Descriptor{Kind::kSensors, u"sensors"};
  }
  if (name == u"screen-wake-lock") {
    return Descriptor{Kind::kWakeLockScreen, u"screen_wake_lock"};
  }
  if (name == u"nfc") {
    if (!Enabled(isolate, RuntimeFeature::kWebNFC)) {
      return type_error("Web NFC is not enabled.");
    }
    return Descriptor{Kind::kNfc, u"nfc"};
  }
  if (name == u"display-capture") {
    return Descriptor{Kind::kDisplayCapture, u"display_capture"};
  }
  if (name == u"clipboard-read" || name == u"clipboard-write") {
    bool allow_without_gesture = false;
    bool allow_without_sanitization = false;
    if (!ReadBoolean(isolate, object, "ClipboardPermissionDescriptor",
                     "allowWithoutGesture", exception_state,
                     &allow_without_gesture)) {
      return std::nullopt;
    }
    {
      bindings::DictionaryConversionScope scope(
          isolate, "ClipboardPermissionDescriptor");
      v8::Local<v8::Value> value;
      if (!Read(isolate, object, scope, "allowWithoutSanitization", &value)) {
        return std::nullopt;
      }
      allow_without_sanitization =
          !value->IsUndefined() && value->BooleanValue(isolate);
    }
    // A write with a gesture and sanitized is its own kind, which Chrome
    // always grants (blink::PermissionDescriptorToPermissionType()).
    if (name == u"clipboard-write" && !allow_without_gesture &&
        !allow_without_sanitization) {
      return Descriptor{Kind::kClipboardSanitizedWrite, u"clipboard_write"};
    }
    return Descriptor{Kind::kClipboardReadWrite, name == u"clipboard-read"
                                                     ? u"clipboard_read"
                                                     : u"clipboard_write"};
  }
  if (name == u"payment-handler") {
    return Descriptor{Kind::kPaymentHandler, u"payment_handler"};
  }
  if (name == u"idle-detection") {
    return Descriptor{Kind::kIdleDetection, u"idle_detection"};
  }
  if (name == u"periodic-background-sync") {
    return Descriptor{Kind::kPeriodicBackgroundSync,
                      u"periodic_background_sync"};
  }
  if (name == u"system-wake-lock") {
    if (!Enabled(isolate, RuntimeFeature::kSystemWakeLock)) {
      return type_error("System Wake Lock is not enabled.");
    }
    return Descriptor{Kind::kWakeLockSystem, u"system_wake_lock"};
  }
  if (name == u"storage-access") {
    return Descriptor{Kind::kStorageAccessGrant, u"storage-access"};
  }
  if (name == u"window-management") {
    return Descriptor{Kind::kWindowManagement, u"window-management"};
  }
  if (name == u"local-fonts") {
    if (!Enabled(isolate, RuntimeFeature::kFontAccess)) {
      return type_error("Local Fonts Access API is not enabled.");
    }
    return Descriptor{Kind::kLocalFonts, u"local_fonts"};
  }
  if (name == u"top-level-storage-access") {
    // The descriptor reads its name again before its own member, as for the
    // other descriptors.
    std::u16string again;
    if (!ReadName(isolate, object, exception_state, &again)) {
      return std::nullopt;
    }
    std::u16string requested_origin;
    {
      bindings::DictionaryConversionScope scope(
          isolate, "TopLevelStorageAccessPermissionDescriptor");
      v8::Local<v8::Value> value;
      if (!Read(isolate, object, scope, "requestedOrigin", &value)) {
        return std::nullopt;
      }
      if (!value->IsUndefined() &&
          !bindings::FromV8USVString(isolate, value, &requested_origin)) {
        return std::nullopt;
      }
    }
    if (!GURL(base::UTF16ToUTF8(requested_origin)).is_valid()) {
      return type_error("The requested origin is invalid.");
    }
    return Descriptor{Kind::kTopLevelStorageAccess,
                      u"top-level-storage-access"};
  }
  if (name == u"captured-surface-control") {
    if (!Enabled(isolate, RuntimeFeature::kCapturedSurfaceControl)) {
      return type_error("The Captured Surface Control API is not enabled.");
    }
    return Descriptor{Kind::kCapturedSurfaceControl,
                      u"captured-surface-control"};
  }
  if (name == u"speaker-selection") {
    if (!Enabled(isolate, RuntimeFeature::kSpeakerSelection)) {
      return type_error("The Speaker Selection API is not enabled.");
    }
    return Descriptor{Kind::kSpeakerSelection, u"speaker-selection"};
  }
  if (name == u"keyboard-lock") {
    return Descriptor{Kind::kKeyboardLock, u"keyboard-lock"};
  }
  if (name == u"pointer-lock") {
    return Descriptor{Kind::kPointerLock, u"pointer-lock"};
  }
  if (name == u"fullscreen") {
    bool allow_without_gesture = false;
    if (!ReadBoolean(isolate, object, "FullscreenPermissionDescriptor",
                     "allowWithoutGesture", exception_state,
                     &allow_without_gesture)) {
      return std::nullopt;
    }
    if (!allow_without_gesture) {
      return type_error(
          "Fullscreen Permission only supports allowWithoutGesture:true.");
    }
    return Descriptor{Kind::kAutomaticFullscreen, u"fullscreen"};
  }
  if (name == u"web-app-installation") {
    if (!Enabled(isolate, RuntimeFeature::kWebAppInstallation)) {
      return type_error("The Web App Install API is not enabled.");
    }
    return Descriptor{Kind::kWebAppInstallation, u"web-app-installation"};
  }
  if (name == u"local-network-access") {
    return Descriptor{Kind::kLocalNetworkAccess, u"local-network-access"};
  }
  if (name == u"local-network") {
    return Descriptor{Kind::kLocalNetwork, u"local-network"};
  }
  return Descriptor{Kind::kLoopbackNetwork, u"loopback-network"};
}

// What Chrome's browser answers for a kind of permission in a profile that
// has granted and denied nothing: a permission context's own answer, or else
// its content setting's default. Every context is restricted to secure
// origins, so a page that is not a secure context gets "denied", but for the
// storage access grant, whose context turns that into "ask".
std::u16string_view StateOf(Kind kind, bool secure) {
  if (!secure) {
    return kind == Kind::kStorageAccessGrant ? u"prompt" : u"denied";
  }
  switch (kind) {
    case Kind::kSensors:
    case Kind::kBackgroundFetch:
    case Kind::kBackgroundSync:
    case Kind::kClipboardSanitizedWrite:
    case Kind::kKeyboardLock:
    case Kind::kPaymentHandler:
    case Kind::kPointerLock:
    case Kind::kWakeLockScreen:
    case Kind::kStorageAccessGrant:
      return u"granted";
    case Kind::kPeriodicBackgroundSync:
    case Kind::kAutomaticFullscreen:
    case Kind::kWakeLockSystem:
    case Kind::kNfc:
      return u"denied";
    default:
      return u"prompt";
  }
}

}  // namespace

// static
Permissions* Permissions::Create() {
  return bindings::MakeGarbageCollected<Permissions>();
}

Permissions::Permissions() = default;
Permissions::~Permissions() = default;

const bindings::WrapperTypeInfo* Permissions::GetWrapperTypeInfo() const {
  return &bindings::V8Permissions::kWrapperTypeInfo;
}

v8::Local<v8::Promise> Permissions::Query(
    v8::Local<v8::Object> permission,
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  const std::optional<Descriptor> descriptor =
      Parse(isolate, permission, exception_state);
  if (!descriptor) {
    return v8::Local<v8::Promise>();
  }
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(isolate->GetCurrentContext())
           .ToLocal(&resolver)) {
    return v8::Local<v8::Promise>();
  }
  const auto [entry, inserted] = names_.try_emplace(
      static_cast<int>(descriptor->kind), std::u16string(descriptor->name));
  const std::u16string_view state =
      StateOf(descriptor->kind, Window::Current().IsSecureContextForBindings());
  // A new PermissionStatus each time, once the browser has answered.
  ResolveFromTask(isolate, resolver,
                  PermissionStatus::Create(entry->second, std::u16string(state))
                      ->ToV8(isolate));
  return resolver->GetPromise();
}

}  // namespace mitto
