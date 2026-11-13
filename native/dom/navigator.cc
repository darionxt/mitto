#include "mitto/dom/navigator.h"

#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_navigator.h"
#include "mitto/dom/geolocation.h"
#include "mitto/dom/lock_manager.h"
#include "mitto/dom/mime_type_array.h"
#include "mitto/dom/navigator_ua_data.h"
#include "mitto/dom/network_information.h"
#include "mitto/dom/permissions.h"
#include "mitto/dom/plugin_array.h"
#include "mitto/dom/script_values.h"
#include "mitto/dom/storage_manager.h"
#include "mitto/dom/window.h"
#include "v8/include/v8-isolate.h"

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

}  // namespace

// static
Navigator* Navigator::Create(const PageStart& start,
                             const PageConnection& connection) {
  return bindings::MakeGarbageCollected<Navigator>(start, connection);
}

Navigator::Navigator(const PageStart& start, const PageConnection& connection)
    : navigator_(start.navigator),
      languages_(ParseAndSanitize(start.navigator.languages, start.locale)),
      processors_(start.processors),
      memory_(start.memory),
      touch_points_(start.touch_points),
      connection_values_(connection),
      effective_type_(start.effective_type) {}

Navigator::~Navigator() = default;

const bindings::WrapperTypeInfo* Navigator::GetWrapperTypeInfo() const {
  return &bindings::V8Navigator::kWrapperTypeInfo;
}

void Navigator::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(languages_array_);
  visitor->Trace(connection_);
  visitor->Trace(plugins_);
  visitor->Trace(mime_types_);
  visitor->Trace(permissions_);
  visitor->Trace(geolocation_);
  visitor->Trace(storage_);
  visitor->Trace(locks_);
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

// LockManager::locks(): the navigator's supplement, which the current window
// owns, since it is that window's navigator a script reads.
mitto::LockManager* Navigator::Locks() {
  if (!locks_) {
    locks_ =
        bindings::MakeGarbageCollected<mitto::LockManager>(Window::Current());
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

size_t Navigator::GeolocationTimerCount() const {
  return geolocation_ ? geolocation_->TimerCount() : 0;
}

std::vector<bindings::ScriptWrappable*> Navigator::GetGamepads(
    bindings::ExceptionState& exception_state) const {
  // device::Gamepads::kItemsLengthCap. The "gamepad" policy allows any page
  // that a Session opens, so nothing throws.
  constexpr size_t kItemsLengthCap = 8;
  return std::vector<bindings::ScriptWrappable*>(kItemsLengthCap, nullptr);
}

}  // namespace mitto
