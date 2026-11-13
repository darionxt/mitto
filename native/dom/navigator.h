// The HTML Standard's Navigator, a window's `navigator`, with what the
// Session's Persona says of the browser, its user agent, client hints and
// languages, and what its Machine says of the computer: processors, memory and
// touch points, and the PDF viewer's plugins. Its other members are Known Gaps.
//
// Upstream: third_party/blink/renderer/core/frame/navigator.cc
// Upstream: third_party/blink/renderer/core/frame/navigator_id.cc
// Upstream: third_party/blink/renderer/core/frame/navigator_language.cc
// Upstream: third_party/blink/renderer/core/frame/navigator_device_memory.cc
// Upstream: third_party/blink/renderer/core/frame/navigator_concurrent_hardware.cc
// Upstream: third_party/blink/renderer/core/frame/navigator_ua.cc
// Upstream: third_party/blink/renderer/core/execution_context/navigator_base.cc
// Upstream: third_party/blink/renderer/modules/plugins/navigator_plugins.cc
// Upstream: third_party/blink/renderer/modules/gamepad/navigator_gamepad.cc
// Upstream: device/gamepad/public/cpp/gamepads.h

#ifndef MITTO_DOM_NAVIGATOR_H_
#define MITTO_DOM_NAVIGATOR_H_

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/page_messages.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-traced-handle.h"

namespace mitto {

class Geolocation;
class MimeTypeArray;
class LockManager;
class NavigatorUAData;
class NetworkInformation;
class Permissions;
class PluginArray;
class StorageManager;

class Navigator final : public bindings::ScriptWrappable {
 public:
  // What `start` says of the browser and the machine, with `connection` for
  // the window's document.
  static Navigator* Create(const PageStart& start,
                           const PageConnection& connection);

  Navigator(const PageStart& start, const PageConnection& connection);
  ~Navigator() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  uint64_t HardwareConcurrency() const;
  bool CookieEnabled() const;
  std::u16string AppCodeName() const;
  std::u16string AppName() const;
  std::u16string AppVersion() const;
  std::u16string Platform() const;
  std::u16string Product() const;
  std::u16string ProductSub() const;
  std::u16string UserAgent() const;
  std::u16string Vendor() const;
  std::u16string VendorSub() const;
  std::u16string Language() const;
  // The same frozen array each time, as [CachedAttribute] keeps it.
  v8::Local<v8::Value> Languages();
  bool OnLine() const;
  bool Webdriver() const;
  bool PdfViewerEnabled() const;
  // The same object each time, [SameObject], made the first time a page asks,
  // as NavigatorPlugins keeps them.
  PluginArray* Plugins();
  MimeTypeArray* MimeTypes();
  bool JavaEnabled() const;
  NetworkInformation* Connection();
  float DeviceMemory() const;
  // A new object each time, as Blink makes one.
  NavigatorUAData* UserAgentData() const;
  // The window's Web Locks, made the first time.
  mitto::LockManager* Locks();
  int32_t MaxTouchPoints() const;
  std::optional<std::u16string> DoNotTrack() const;
  // [SameObject], each made the first time a page asks.
  mitto::Permissions* Permissions();
  mitto::Geolocation* Geolocation();
  StorageManager* Storage();
  // How many of navigator.geolocation's requests wait on a timer.
  size_t GeolocationTimerCount() const;
  // A new array each time, of kItemsLengthCap nulls: no gamepad is there.
  std::vector<bindings::ScriptWrappable*> GetGamepads(
      bindings::ExceptionState& exception_state) const;
  // The same promise each time, which resolves from a task of its own with
  // the BatteryManager once the device service gives its first reading.
  v8::Local<v8::Promise> GetBattery();
  // NavigatorCPUPerformance: Chrome's tier of the processors, from their
  // number, as content::cpu_performance gives it for an AMD or Intel CPU of
  // none of the models it singles out.
  uint16_t CpuPerformance() const;

 private:
  const PageNavigator navigator_;
  // navigator.languages, from the languages of Chrome's setting, or the
  // Persona's locale for none.
  const std::vector<std::u16string> languages_;
  const int processors_;
  const int memory_;
  const int touch_points_;
  const PageConnection connection_values_;
  const std::string effective_type_;
  v8::TracedReference<v8::Array> languages_array_;
  v8::TracedReference<v8::Promise> battery_promise_;
  cppgc::Member<NetworkInformation> connection_;
  cppgc::Member<PluginArray> plugins_;
  cppgc::Member<MimeTypeArray> mime_types_;
  cppgc::Member<mitto::Permissions> permissions_;
  cppgc::Member<mitto::Geolocation> geolocation_;
  cppgc::Member<StorageManager> storage_;
  cppgc::Member<mitto::LockManager> locks_;
};

}  // namespace mitto

#endif  // MITTO_DOM_NAVIGATOR_H_
