// The HTML Standard's Window, a Page's global object, with `window`, `self`,
// `document`, `performance`, the timers, queueMicrotask(), fetch(), atob() and
// btoa(), `navigator`, `screen` and `crypto`, and the window's place and size
// as the Session's Machine has them; its other members are Known Gaps.
//
// A context's global object and its global proxy both wrap the Window, as in
// Blink, so that a call whose receiver is either finds it. The window's
// wrapper is the global proxy, which `window`, `self` and globalThis are.
//
// Upstream: third_party/blink/renderer/core/frame/dom_window.h
// Upstream: third_party/blink/renderer/core/frame/local_dom_window.h
// Upstream: third_party/blink/renderer/bindings/core/v8/local_window_proxy.cc
// Upstream: third_party/blink/renderer/modules/crypto/global_crypto.cc

#ifndef MITTO_DOM_WINDOW_H_
#define MITTO_DOM_WINDOW_H_

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <map>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/dom/window_properties.h"
#include "mitto/page_messages.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-value.h"

namespace mitto {

class BarProp;
class CrashReportContext;
class Crypto;
class CustomElementRegistry;
class DOMTimerCoordinator;
class Document;
class DocumentLoadTiming;
class Fetcher;
class History;
class Location;
class MediaQueryList;
class Navigation;
class Navigator;
class Performance;
class Screen;
class StyleMedia;
class Viewport;
class VisualViewport;

class Window final : public WindowProperties {
 public:
  static Window* Create();
  // The Window of the isolate's current context.
  static Window& Current();

  Window();
  ~Window() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  Window* WindowForBinding() { return this; }
  Window* Self() { return this; }
  mitto::Document* Document() const { return document_.Get(); }
  mitto::Performance* Performance();
  int32_t SetTimeout(v8::Local<v8::Function> handler,
                     int32_t timeout,
                     const std::vector<v8::Local<v8::Value>>& arguments);
  int32_t SetTimeout(const std::u16string& handler,
                     int32_t timeout,
                     const std::vector<v8::Local<v8::Value>>& arguments,
                     bindings::ExceptionState& exception_state);
  void ClearTimeout(int32_t handle);
  int32_t SetInterval(v8::Local<v8::Function> handler,
                      int32_t timeout,
                      const std::vector<v8::Local<v8::Value>>& arguments);
  int32_t SetInterval(const std::u16string& handler,
                      int32_t timeout,
                      const std::vector<v8::Local<v8::Value>>& arguments,
                      bindings::ExceptionState& exception_state);
  void ClearInterval(int32_t handle);
  void QueueMicrotask(v8::Local<v8::Function> callback);
  std::u16string Btoa(const std::u16string& data,
                      bindings::ExceptionState& exception_state);
  std::u16string Atob(const std::u16string& data,
                      bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Fetch(const bindings::RequestOrUSVString& input,
                               const bindings::RequestInit& init,
                               bindings::ExceptionState& exception_state);
  mitto::Navigator* Navigator();
  mitto::Navigator* ClientInformation() { return Navigator(); }
  mitto::Screen* Screen();
  int32_t InnerWidth() const { return start_.viewport_width; }
  int32_t InnerHeight() const { return start_.viewport_height; }
  int32_t ScreenX() const { return start_.window_x; }
  int32_t ScreenLeft() const { return ScreenX(); }
  int32_t ScreenY() const { return start_.window_y; }
  int32_t ScreenTop() const { return ScreenY(); }
  int32_t OuterWidth() const { return start_.window_width; }
  int32_t OuterHeight() const { return start_.window_height; }
  double DevicePixelRatio() const { return start_.scale; }
  MediaQueryList* MatchMedia(const std::u16string& query);
  // A top-level window of a tab that a Session opened: its own top, parent
  // and frames, with no opener.
  Window* Frames() { return this; }
  Window* Top() { return this; }
  Window* Parent() { return this; }
  v8::Local<v8::Value> OpenerForBindings() const;
  void SetOpenerForBindings(v8::Local<v8::Value> opener,
                            bindings::ExceptionState& exception_state);
  std::u16string Origin() const;
  bool IsSecureContextForBindings() const;
  // Chrome keys agent clusters by origin by default.
  bool OriginAgentCluster() const { return true; }
  bool OffscreenBuffering() const { return true; }
  CrashReportContext* CrashReport();
  mitto::Location* Location();
  mitto::History* History();
  BarProp* Locationbar() { return Bar(kLocationbar); }
  BarProp* Menubar() { return Bar(kMenubar); }
  BarProp* Personalbar() { return Bar(kPersonalbar); }
  BarProp* Scrollbars() { return Bar(kScrollbars); }
  BarProp* Statusbar() { return Bar(kStatusbar); }
  BarProp* Toolbar() { return Bar(kToolbar); }
  mitto::VisualViewport* VisualViewport();
  mitto::Viewport* Viewport();
  mitto::StyleMedia* StyleMedia();
  mitto::Navigation* Navigation();
  CustomElementRegistry* CustomElements();
  mitto::Crypto* Crypto();

  // What the Page uses.
  mitto::Document& GetDocument() const { return *document_; }
  // Makes `document` the window's, and the window its document's.
  void SetDocument(mitto::Document* document);
  // Makes the global proxy and the global object of `context`, which was made
  // from Window's template, this window's wrappers, as Blink's
  // LocalWindowProxy::SetupWindowPrototypeChain() does.
  void AssociateWithContext(v8::Isolate* isolate,
                            v8::Local<v8::Context> context);
  // The timers of the window's context, which the Page program keeps, and
  // which go with it. Without them, as once the context is gone, a timer set
  // is never run, and its ID is 0, as Chrome gives in a destroyed context.
  // Setting none stops the timers of AbortSignal.timeout() too.
  void SetTimers(DOMTimerCoordinator* timers);
  // How many timers are set, those of AbortSignal.timeout() among them.
  size_t TimerCount() const;
  // What fetch() asks the Session through. Without one, it fails.
  void SetFetcher(Fetcher* fetcher) { fetcher_ = fetcher; }
  // Whether the Page still runs the window's context: it gives the window its
  // timers until the context goes.
  bool HasContext() const { return timers_ != nullptr; }
  // The timer of an AbortSignal.timeout(), which counts among the window's
  // timers until RemoveSignalTimer() takes it back with the ID that this
  // returns. Its `cancel` runs once the Page lets the context go, as Blink's
  // ContextDestroyed() cancels the signal's task.
  uint64_t AddSignalTimer(base::OnceClosure cancel);
  void RemoveSignalTimer(uint64_t id);
  // What the window reads of the browser and the machine, from the Page's
  // start, and what navigator.connection gives its document. Without them, as
  // in the Page program given a script, it reads what a default PageStart
  // holds.
  void SetStart(const PageStart& start, const PageConnection& connection);
  // What the window reads of the browser and the machine.
  const PageStart& Start() const { return start_; }
  // How the window's document loads, which performance.timing and the
  // navigation's entry read. Without one, as in the Page program given a
  // script, the window starts one at its time origin.
  void SetDocumentLoadTiming(DocumentLoadTiming* timing);
  DocumentLoadTiming& GetDocumentLoadTiming();
  // Where the console messages of the DOM go, such as a warning of Blink's.
  // Without one, nowhere.
  using ConsoleSink =
      base::RepeatingCallback<void(ConsoleLevel level, std::string text)>;
  void SetConsoleSink(ConsoleSink sink) { console_sink_ = std::move(sink); }
  // ExecutionContext::AddConsoleMessage().
  void AddConsoleMessage(ConsoleLevel level, std::string text);

 private:
  // The window's bars, each an object of its own, as Blink's are.
  enum BarKind : size_t {
    kLocationbar,
    kMenubar,
    kPersonalbar,
    kScrollbars,
    kStatusbar,
    kToolbar,
    kBarCount,
  };

  BarProp* Bar(BarKind kind);

  cppgc::Member<mitto::Document> document_;
  cppgc::Member<mitto::Performance> performance_;
  cppgc::Member<mitto::Navigator> navigator_;
  cppgc::Member<mitto::Screen> screen_;
  cppgc::Member<CrashReportContext> crash_report_;
  cppgc::Member<mitto::Location> location_;
  cppgc::Member<mitto::History> history_;
  // In the object, not in a std::vector's buffer, for cppgc to trace.
  std::array<cppgc::Member<BarProp>, kBarCount> bars_;
  cppgc::Member<mitto::VisualViewport> visual_viewport_;
  cppgc::Member<mitto::Viewport> viewport_;
  cppgc::Member<mitto::StyleMedia> style_media_;
  cppgc::Member<mitto::Navigation> navigation_;
  cppgc::Member<CustomElementRegistry> custom_elements_;
  cppgc::Member<mitto::Crypto> crypto_;
  cppgc::Member<DocumentLoadTiming> document_load_timing_;
  ConsoleSink console_sink_;
  PageStart start_;
  PageConnection connection_;
  raw_ptr<DOMTimerCoordinator> timers_ = nullptr;
  raw_ptr<Fetcher> fetcher_ = nullptr;
  std::map<uint64_t, base::OnceClosure> signal_timers_;
  uint64_t next_signal_timer_ = 0;
};

}  // namespace mitto

#endif  // MITTO_DOM_WINDOW_H_
