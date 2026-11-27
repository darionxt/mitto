#include "mitto/dom/navigator.h"

#include <limits>
#include <memory>
#include <utility>
#include <variant>

#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/strings/strcat.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_navigator.h"
#include "mitto/dom/battery_manager.h"
#include "mitto/dom/blob.h"
#include "mitto/dom/blob_data.h"
#include "mitto/dom/credentials_container.h"
#include "mitto/dom/document.h"
#include "mitto/dom/fetcher.h"
#include "mitto/dom/file.h"
#include "mitto/dom/form_data.h"
#include "mitto/dom/geolocation.h"
#include "mitto/dom/keyboard.h"
#include "mitto/dom/lock_manager.h"
#include "mitto/dom/media_devices.h"
#include "mitto/dom/mime_type_array.h"
#include "mitto/dom/navigator_ua_data.h"
#include "mitto/dom/network_information.h"
#include "mitto/dom/parsed_content_type.h"
#include "mitto/dom/permissions.h"
#include "mitto/dom/plugin_array.h"
#include "mitto/dom/readable_stream.h"
#include "mitto/dom/script_values.h"
#include "mitto/dom/storage_bucket_manager.h"
#include "mitto/dom/storage_manager.h"
#include "mitto/dom/url_search_params.h"
#include "mitto/dom/window.h"
#include "mitto/header_rules.h"
#include "url/gurl.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"

namespace mitto {
namespace {

// navigator.languages from the languages of Chrome's setting, as
// ParseAndSanitize() makes them: split at commas, each without the white space
// around it and with an underscore after its first two letters made a hyphen,
// and DefaultLanguage(), the browser's own locale, for none.
// Upstream: third_party/blink/renderer/core/frame/navigator_language.cc
std::vector<std::u16string> ParseAndSanitize(
    const std::string& accept_languages,
    const std::string& locale) {
  std::vector<std::u16string> languages;
  for (std::string_view token :
       base::SplitStringPiece(accept_languages, ",", base::KEEP_WHITESPACE,
                              base::SPLIT_WANT_NONEMPTY)) {
    std::string language(base::TrimWhitespaceASCII(token, base::TRIM_ALL));
    if (language.size() >= 3 && language[2] == '_') {
      language[2] = '-';
    }
    languages.push_back(base::UTF8ToUTF16(language));
  }
  if (languages.empty()) {
    languages.push_back(base::UTF8ToUTF16(locale));
  }
  return languages;
}

// A beacon's body, as Blink's BeaconData subclasses serialize theirs into the
// request: its bytes, its Content-Type if it has one, and whether it goes in
// mode cors, which only a Blob of a type that is not CORS-safelisted does.
struct BeaconBody {
  std::string bytes;
  std::optional<std::string> content_type;
  bool cors = false;
  // A Blob or buffer larger than the keepalive quota, which never fits in it,
  // whose bytes are not copied.
  bool past_quota = false;
};

// The size that Blink's beacons of a buffer cannot pass.
constexpr char kBeaconTooLong[] =
    "The data provided to sendBeacon() exceeds the maximally possible length, "
    "which is 4294967295.";

using BeaconData = bindings::
    ReadableStreamOrBlobOrArrayBufferOrArrayBufferViewOrFormDataOrURLSearchParamsOrUSVString;

// Serializes `data`, or nothing for none, which goes as an empty body with no
// Content-Type, as Blink's BeaconString of a null string.
std::optional<BeaconBody> SerializeBeacon(
    const std::optional<BeaconData>& data,
    bindings::ExceptionState& exception_state) {
  BeaconBody body;
  if (!data) {
    return body;
  }
  if (std::holds_alternative<ReadableStream*>(*data)) {
    exception_state.ThrowTypeError(
        "sendBeacon cannot have a ReadableStream body.");
    return std::nullopt;
  }
  if (Blob* const* const blob = std::get_if<Blob*>(&*data)) {
    const BlobDataHandle& handle = *(*blob)->GetBlobDataHandle();
    body.past_quota = handle.size() > kKeepaliveInflightBytesQuota;
    if (!body.past_quota) {
      const base::span<const uint8_t> bytes = handle.Bytes();
      body.bytes.assign(bytes.begin(), bytes.end());
    }
    const std::string& type = handle.GetType();
    if (!type.empty() && ParsedContentType(base::UTF8ToUTF16(type)).IsValid()) {
      // BeaconBlob::Serialize(): a type of any length that is safelisted.
      body.cors = !IsCorsSafelistedContentType(type);
      body.content_type = type;
    }
    return body;
  }
  if (FormData* const* const form_data = std::get_if<FormData*>(&*data)) {
    // Its files alone past the keepalive quota, which the multipart body only
    // adds to, are not copied into it.
    uint64_t file_bytes = 0;
    for (size_t index = 0; index < (*form_data)->size(); ++index) {
      if (const File* const file = (*form_data)->EntryAt(index)->file()) {
        file_bytes += file->GetBlobDataHandle()->size();
      }
    }
    if (file_bytes > kKeepaliveInflightBytesQuota) {
      body.past_quota = true;
      return body;
    }
    FormData::MultipartBody multipart = (*form_data)->EncodeMultiPartFormData();
    body.content_type = multipart.ContentType();
    body.bytes = std::move(multipart.bytes);
    return body;
  }
  if (URLSearchParams* const* const params =
          std::get_if<URLSearchParams*>(&*data)) {
    body.bytes = base::UTF16ToUTF8((*params)->ToString());
    body.content_type = "application/x-www-form-urlencoded;charset=UTF-8";
    return body;
  }
  if (const auto* const buffer =
          std::get_if<v8::Local<v8::ArrayBuffer>>(&*data)) {
    const std::shared_ptr<v8::BackingStore> store =
        (*buffer)->GetBackingStore();
    if (store->ByteLength() > std::numeric_limits<uint32_t>::max()) {
      exception_state.ThrowRangeError(kBeaconTooLong);
      return std::nullopt;
    }
    body.past_quota = store->ByteLength() > kKeepaliveInflightBytesQuota;
    if (!body.past_quota) {
      body.bytes.assign(static_cast<const char*>(store->Data()),
                        store->ByteLength());
    }
    return body;
  }
  if (const auto* const view =
          std::get_if<v8::Local<v8::ArrayBufferView>>(&*data)) {
    if ((*view)->ByteLength() > std::numeric_limits<uint32_t>::max()) {
      exception_state.ThrowRangeError(kBeaconTooLong);
      return std::nullopt;
    }
    body.past_quota = (*view)->ByteLength() > kKeepaliveInflightBytesQuota;
    if (!body.past_quota) {
      body.bytes.resize((*view)->ByteLength());
      (*view)->CopyContents(body.bytes.data(), body.bytes.size());
    }
    return body;
  }
  body.bytes = base::UTF16ToUTF8(std::get<std::u16string>(*data));
  body.content_type = "text/plain;charset=UTF-8";
  return body;
}

// What a beacon's request tells, of which a script sees nothing: its body is
// taken as it comes, so that the Session reads it all, and a request that
// fails before Fetch() returns, as one past the keepalive quota does, sets
// `failed`.
class BeaconClient final : public FetchClient {
 public:
  explicit BeaconClient(scoped_refptr<base::RefCountedData<bool>> failed)
      : failed_(std::move(failed)) {}

  void OnStarted(FetchController& controller) override {
    controller_ = &controller;
  }
  void OnResponseStarted(const FetchResponseHead& head) override {}
  void OnResponseBody(std::string bytes) override {
    controller_->TookBody(bytes.size());
  }
  void OnResponseEnded(int net_error) override {
    if (net_error != 0) {
      failed_->data = true;
    }
  }

 private:
  raw_ptr<FetchController> controller_ = nullptr;
  const scoped_refptr<base::RefCountedData<bool>> failed_;
};

}  // namespace

// static
Navigator* Navigator::Create(const PageStart& start,
                             const PageConnection& connection,
                             Window* window) {
  return bindings::MakeGarbageCollected<Navigator>(start, connection, window);
}

Navigator::Navigator(const PageStart& start,
                     const PageConnection& connection,
                     Window* window)
    : window_(window),
      navigator_(start.navigator),
      languages_(ParseAndSanitize(start.navigator.languages, start.locale)),
      processors_(start.processors),
      memory_(start.memory),
      touch_points_(start.touch_points),
      connection_values_(connection),
      media_devices_(start.media_devices),
      effective_type_(start.effective_type) {}

Navigator::~Navigator() = default;

const bindings::WrapperTypeInfo* Navigator::GetWrapperTypeInfo() const {
  return &bindings::V8Navigator::kWrapperTypeInfo;
}

void Navigator::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(window_);
  visitor->Trace(languages_array_);
  visitor->Trace(battery_promise_);
  visitor->Trace(connection_);
  visitor->Trace(plugins_);
  visitor->Trace(mime_types_);
  visitor->Trace(permissions_);
  visitor->Trace(geolocation_);
  visitor->Trace(storage_);
  visitor->Trace(storage_buckets_);
  visitor->Trace(keyboard_);
  visitor->Trace(media_devices_object_);
  visitor->Trace(locks_);
  visitor->Trace(credentials_);
}

uint64_t Navigator::HardwareConcurrency() const {
  return static_cast<uint64_t>(processors_);
}

bool Navigator::CookieEnabled() const {
  // Chrome's settings allow cookies by default.
  return true;
}

std::u16string Navigator::AppCodeName() const {
  return u"Mozilla";
}

std::u16string Navigator::AppName() const {
  return u"Netscape";
}

std::u16string Navigator::AppVersion() const {
  // Version is everything in the user agent string past the "Mozilla/"
  // prefix.
  const std::u16string agent = UserAgent();
  const size_t slash = agent.find(u'/');
  return slash == std::u16string::npos ? agent : agent.substr(slash + 1);
}

std::u16string Navigator::Platform() const {
  return base::UTF8ToUTF16(navigator_.platform);
}

std::u16string Navigator::Product() const {
  return u"Gecko";
}

std::u16string Navigator::ProductSub() const {
  return u"20030107";
}

std::u16string Navigator::UserAgent() const {
  return base::UTF8ToUTF16(navigator_.user_agent);
}

std::u16string Navigator::Vendor() const {
  return u"Google Inc.";
}

std::u16string Navigator::VendorSub() const {
  return u"";
}

std::u16string Navigator::Language() const {
  return languages_.front();
}

v8::Local<v8::Value> Navigator::Languages() {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (languages_array_.IsEmpty()) {
    std::vector<v8::Local<v8::Value>> items;
    for (const std::u16string& language : languages_) {
      items.push_back(bindings::ToV8String(isolate, language));
    }
    languages_array_.Reset(isolate, FrozenArray(isolate, items));
  }
  return languages_array_.Get(isolate);
}

bool Navigator::OnLine() const {
  return true;
}

bool Navigator::Webdriver() const {
  // A Page is never started for automation, which turns
  // AutomationControlled on.
  return false;
}

bool Navigator::PdfViewerEnabled() const {
  // Chrome's PDF viewer handles application/pdf in a new profile, so
  // navigator.plugins lists it.
  return true;
}

PluginArray* Navigator::Plugins() {
  if (!plugins_) {
    plugins_ = PluginArray::Create();
  }
  return plugins_.Get();
}

MimeTypeArray* Navigator::MimeTypes() {
  if (!mime_types_) {
    mime_types_ = MimeTypeArray::Create(Plugins());
  }
  return mime_types_.Get();
}

bool Navigator::JavaEnabled() const {
  return false;
}

NetworkInformation* Navigator::Connection() {
  if (!connection_) {
    connection_ =
        NetworkInformation::Create(connection_values_, effective_type_);
  }
  return connection_.Get();
}

float Navigator::DeviceMemory() const {
  return static_cast<float>(memory_);
}

NavigatorUAData* Navigator::UserAgentData() const {
  return NavigatorUAData::Create(navigator_.user_agent_data);
}

// LockManager::locks(): the navigator's supplement, of its window, whichever
// window's script reads it.
mitto::LockManager* Navigator::Locks() {
  if (!locks_) {
    locks_ = bindings::MakeGarbageCollected<mitto::LockManager>(*window_);
  }
  return locks_.Get();
}

int32_t Navigator::MaxTouchPoints() const {
  return touch_points_;
}

std::optional<std::u16string> Navigator::DoNotTrack() const {
  // Chrome's setting that sends DNT is off by default.
  return std::nullopt;
}

mitto::Permissions* Navigator::Permissions() {
  if (!permissions_) {
    permissions_ = mitto::Permissions::Create();
  }
  return permissions_.Get();
}

mitto::Geolocation* Navigator::Geolocation() {
  if (!geolocation_) {
    geolocation_ = mitto::Geolocation::Create();
  }
  return geolocation_.Get();
}

StorageManager* Navigator::Storage() {
  if (!storage_) {
    storage_ = StorageManager::Create();
  }
  return storage_.Get();
}

// StorageBucketManager::storageBuckets(): the navigator's supplement.
StorageBucketManager* Navigator::StorageBuckets() {
  if (!storage_buckets_) {
    storage_buckets_ = StorageBucketManager::Create();
  }
  return storage_buckets_.Get();
}

bool Navigator::HasPublicKeyRequest() const {
  return credentials_ && credentials_->HasPublicKeyRequest();
}

CredentialsContainer* Navigator::Credentials() {
  if (!credentials_) {
    credentials_ = CredentialsContainer::Create();
  }
  return credentials_.Get();
}

size_t Navigator::GeolocationTimerCount() const {
  return geolocation_ ? geolocation_->TimerCount() : 0;
}

// Upstream: third_party/blink/renderer/modules/battery/battery_manager.cc
v8::Local<v8::Promise> Navigator::GetBattery() {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (battery_promise_.IsEmpty()) {
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(isolate->GetCurrentContext())
             .ToLocal(&resolver)) {
      return v8::Local<v8::Promise>();
    }
    ResolveFromTask(isolate, resolver, BatteryManager::Create()->ToV8(isolate));
    battery_promise_.Reset(isolate, resolver->GetPromise());
  }
  return battery_promise_.Get(isolate);
}

mitto::Keyboard* Navigator::Keyboard() {
  if (!keyboard_) {
    keyboard_ = mitto::Keyboard::Create();
  }
  return keyboard_.Get();
}

mitto::MediaDevices* Navigator::MediaDevices() {
  if (!media_devices_object_) {
    media_devices_object_ = mitto::MediaDevices::Create(media_devices_);
  }
  return media_devices_object_.Get();
}

// Upstream: content/browser/cpu_performance/cpu_performance.cc
uint16_t Navigator::CpuPerformance() const {
  // GetTierFromCpuInfo(): low, mid, high and ultra, by the logical
  // processors.
  if (processors_ <= 0) {
    return 0;
  }
  if (processors_ <= 1) {
    return 1;
  }
  if (processors_ <= 4) {
    return 2;
  }
  if (processors_ <= 10) {
    return 3;
  }
  return 4;
}

std::vector<bindings::ScriptWrappable*> Navigator::GetGamepads(
    bindings::ExceptionState& exception_state) const {
  // NavigatorGamepad::getGamepads(), which asks no policy once its window's
  // frame has gone.
  Window& window = Window::OfReceiver();
  if (window.GetFrame() && !window.GetDocument().IsFeatureEnabled("gamepad")) {
    exception_state.ThrowDOMException("SecurityError",
                                      "Access to the feature \"gamepad\" is "
                                      "disallowed by permissions policy.");
    return {};
  }
  // device::Gamepads::kItemsLengthCap.
  constexpr size_t kItemsLengthCap = 8;
  return std::vector<bindings::ScriptWrappable*>(kItemsLengthCap, nullptr);
}

bool Navigator::SendBeacon(const std::u16string& url_string,
                           const std::optional<BeaconData>& data,
                           bindings::ExceptionState& exception_state) {
  // The navigator's window, in whose realm it runs, as Blink's bindings give
  // a non-static operation its receiver's ScriptState, so that a sendBeacon()
  // that another window borrows resolves, checks and goes as this one's.
  if (!window_) {
    return false;
  }
  Window& window = *window_;
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::Context> context;
  std::optional<v8::Context::Scope> context_scope;
  if (window.HasContext() &&
      window.ToV8(isolate)->GetCreationContext(isolate).ToLocal(&context)) {
    context_scope.emplace(context);
  }
  const GURL url = window.CompleteUrl(url_string);
  if (!url.is_valid()) {
    exception_state.ThrowTypeError(
        "The URL argument is ill-formed or unsupported.");
    return false;
  }
  if (!url.SchemeIsHTTPOrHTTPS()) {
    exception_state.ThrowTypeError("Beacons are only supported over HTTP(S).");
    return false;
  }
  // CanSendBeacon(): a detached window sends none, before its data is read.
  Fetcher* const fetcher = window.GetFetcher();
  if (!fetcher) {
    return false;
  }
  std::optional<BeaconBody> body = SerializeBeacon(data, exception_state);
  if (!body) {
    return false;
  }
  // One that never fits in the keepalive quota fails as Fetch() fails it.
  if (body->past_quota) {
    return false;
  }
  // PingLoader's SendBeaconCommon(): a keepalive POST, of request context
  // BEACON and initiator type "beacon", with credentials, in mode no-cors
  // but for the Blob that BeaconBlob puts in mode cors, through the
  // document's ResourceFetcher, which fails one past its keepalive quota.
  FetchRequestData request;
  request.method = "POST";
  request.url = url.spec();
  if (body->content_type) {
    request.headers =
        base::StrCat({"Content-Type: ", *body->content_type, "\n"});
  }
  request.body = std::move(body->bytes);
  request.mode = body->cors ? FetchMode::kCors : FetchMode::kNoCors;
  request.credentials = FetchCredentialsMode::kInclude;
  request.referrer_policy = window.GetDocument().GetReferrerPolicy();
  request.keepalive = true;
  request.is_beacon = true;
  request.initiator_type = u"beacon";
  auto failed = base::MakeRefCounted<base::RefCountedData<bool>>(false);
  fetcher->Fetch(std::move(request), std::make_unique<BeaconClient>(failed));
  return !failed->data;
}

}  // namespace mitto
