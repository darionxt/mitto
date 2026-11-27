// The HTML Standard's Window, a Page's global object, with `window`, `self`,
// `document`, `performance`, animation frame callbacks, fetch(), `navigator`,
// `screen` and `crypto`, and the window's place and size as the Session's
// Machine has them, beside what every ExecutionContext has, the timers,
// queueMicrotask(), atob() and btoa(); its other members are Known Gaps.
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
#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "build/build_config.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/window_properties.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-value.h"

#if BUILDFLAG(IS_WIN)
// base/win/windows_types.h defines PostMessage as PostMessageW, as <windows.h>
// does, which would rename the member wherever it came first. Once its guard
// has seen it, the name stays the member's.
#include "base/win/windows_types.h"
#undef PostMessage
#endif  // BUILDFLAG(IS_WIN)

namespace mitto {

class AnimationFrameController;
class BarProp;
class CrashReportContext;
struct HistoryItem;
class CacheStorage;
class CacheStorageHost;
class IndexedDbHost;
class StorageBucketHost;
class CookieStore;
class Storage;
class Crypto;
class CustomElementRegistry;
class DeferredFetches;
class Document;
class DocumentLoadTiming;
class Element;
class Event;
class FetchLaterResult;
class Frame;
class History;
class IDBFactory;
class IdleTaskController;
class Location;
class MediaQueryList;
class Modulator;
class Navigation;
class NavigationClient;
class Navigator;
class Performance;
struct RendererProcess;
class Scheduler;
class Screen;
class StyleMedia;
class Viewport;
class VisualViewport;

class Window final : public WindowProperties, public ExecutionContext {
 public:
  static Window* Create();
  // The Window of the isolate's current context.
  static Window& Current();
  // The window of `context`, as ExecutionContext::From() finds it, or null
  // for a context of none or of a worker.
  static Window* From(v8::Isolate* isolate, v8::Local<v8::Context> context);
  // The windows of the entered and the incumbent contexts, as Blink's
  // EnteredDOMWindow() and IncumbentDOMWindow() give them: the window whose
  // script the task entered, and the one whose script made the call. Null
  // outside a context.
  static Window* Entered();
  static Window* Incumbent();
  // Whether `accessing_context` may access the window that `accessed_object`,
  // a global proxy, gives, as bind_gen's AccessCheckCallback() of Window asks
  // BindingSecurity::ShouldAllowAccessTo(): for a window of the same origin.
  // V8 asks for contexts of different security tokens, such as that of a
  // window that its frame navigated away from, which loses its token.
  static bool AccessCheckCallback(v8::Local<v8::Context> accessing_context,
                                  v8::Local<v8::Object> accessed_object,
                                  v8::Local<v8::Value> data);

  Window();
  ~Window() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  Window* WindowForBinding() { return this; }
  Window* Self() { return this; }
  mitto::Document* Document() const { return document_.Get(); }
  mitto::Performance* Performance() override;
  // The document's.
  CacheStorageHost* GetCacheStorageHost() const override;
  IndexedDbHost* GetIndexedDbHost() const override;
  StorageBucketHost* GetStorageBucketHost() const override;
  // DOMWindowFileSystem's: the file system of the document's origin, from a
  // task of its own, as Chrome's browser opens one for a new profile, or an
  // error for an opaque origin or a type other than TEMPORARY and PERSISTENT.
  void WebkitRequestFileSystem(uint16_t type,
                               int64_t size,
                               v8::Local<v8::Object> success_callback,
                               v8::Local<v8::Object> error_callback);
  mitto::Navigator* Navigator();
  // Whether the browser holds a WebAuthn request of the window's document.
  bool HasPublicKeyRequest() const;
  mitto::Navigator* ClientInformation() { return Navigator(); }
  mitto::Screen* Screen();
  int32_t InnerWidth() const { return Start().viewport_width; }
  int32_t InnerHeight() const { return Start().viewport_height; }
  int32_t ScreenX() const { return Start().window_x; }
  int32_t ScreenLeft() const { return ScreenX(); }
  int32_t ScreenY() const { return Start().window_y; }
  int32_t ScreenTop() const { return ScreenY(); }
  int32_t OuterWidth() const { return Start().window_width; }
  int32_t OuterHeight() const { return Start().window_height; }
  double DevicePixelRatio() const { return Start().scale; }
  MediaQueryList* MatchMedia(const std::u16string& query);
  // The window's place in its frame's tree, as Blink's DOMWindow gives it:
  // itself for frames, its top-level frame's window, its parent frame's, or
  // its own for a top-level window, which has no opener. A window that has
  // lost its frame, as one whose document a navigation replaced, has neither
  // top nor parent, and no children. The Page program given a script has no
  // frame, and its window is a top-level one.
  Window* Frames() { return this; }
  uint32_t Length() const;
  Window* Top();
  Window* Parent();
  // DOMWindow::AnonymousIndexedGetter(): window[index], the window of the
  // frame's child of that index, in the order the children were made.
  Window* AnonymousIndexedGetter(uint32_t index);
  // LocalDOMWindow::frameElement(): the element that holds the window's frame
  // in its parent's document, or null for a top-level window, and for a caller
  // of another origin than that document, as [CheckSecurity=ReturnValue]
  // checks it.
  Element* FrameElement() const;
  // DOMWindow::closed(): whether the window has lost its frame, as one whose
  // frame went or whose document a navigation replaced. The Page program's own
  // window, which has no frame, is open.
  bool Closed() const { return !frame_ && !HasContext(); }
  // The name of the window's browsing context, which it keeps across its
  // documents, and which a window without a frame does not have.
  std::u16string Name() const;
  void SetName(const std::u16string& name);
  v8::Local<v8::Value> OpenerForBindings() const;
  void SetOpenerForBindings(v8::Local<v8::Value> opener,
                            bindings::ExceptionState& exception_state);
  std::u16string Origin() const override;
  bool IsSecureContextForBindings() const override;
  // WindowOrWorkerGlobalScope::crossOriginIsolated().
  bool CrossOriginIsolated() const { return CrossOriginIsolatedCapability(); }
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
  uint32_t RequestIdleCallback(v8::Local<v8::Function> callback,
                               const bindings::IdleRequestOptions& options);
  void CancelIdleCallback(uint32_t handle);
  int32_t RequestAnimationFrame(v8::Local<v8::Function> callback);
  int32_t WebkitRequestAnimationFrame(v8::Local<v8::Function> callback);
  // webkitCancelAnimationFrame() too.
  void CancelAnimationFrame(int32_t handle);
  // The event that a listener of the window's context is called for, or
  // undefined outside one.
  v8::Local<v8::Value> Event() const;
  mitto::Crypto* Crypto();
  // The window's scheduler, made the first time.
  mitto::Scheduler* Scheduler();
  // The window's cookie store, made the first time.
  mitto::CookieStore* CookieStore();
  // The window's CookieStore, if a script has asked for it.
  mitto::CookieStore* GetCookieStoreIfExists() const {
    return cookie_store_.Get();
  }
  // GlobalCacheStorage::caches(): the window's CacheStorage, made the first
  // time, or null after throwing a SecurityError for a document whose origin
  // may not reach caches.
  mitto::CacheStorage* Caches(bindings::ExceptionState& exception_state);
  // GlobalIndexedDB::indexedDB(): the window's IDBFactory, made the first
  // time, which a document whose origin may not reach IndexedDB has too: its
  // calls throw.
  IDBFactory* IndexedDB();
  // fetchLater(), as fetch_later.h says.
  FetchLaterResult* FetchLater(const bindings::RequestOrUSVString& input,
                               const bindings::DeferredRequestInit& init,
                               bindings::ExceptionState& exception_state);
  // The deferred requests of fetchLater() that have not gone yet, made the
  // first time.
  const scoped_refptr<DeferredFetches>& GetDeferredFetches();
  // Those requests, or null before the first.
  const DeferredFetches* FindDeferredFetches() const {
    return deferred_fetches_.get();
  }
  // WindowLocalStorage and WindowSessionStorage, in dom_window_storage.cc.
  mitto::Storage* LocalStorage(bindings::ExceptionState& exception_state);
  mitto::Storage* SessionStorage(bindings::ExceptionState& exception_state);
  // window_messaging.cc: a structured clone of `value`, and a message to the
  // window itself, which frames will extend to other windows.
  v8::Local<v8::Value> StructuredClone(
      v8::Local<v8::Value> value,
      const bindings::StructuredSerializeOptions& options,
      bindings::ExceptionState& exception_state);
  void PostMessage(v8::Local<v8::Value> message,
                   const std::u16string& target_origin,
                   const std::vector<v8::Local<v8::Object>>& transfer,
                   bindings::ExceptionState& exception_state);
  void PostMessage(v8::Local<v8::Value> message,
                   const bindings::WindowPostMessageOptions& options,
                   bindings::ExceptionState& exception_state);

  // LocalDOMWindow::open() (window_open.cc): navigates the window to `url`
  // for a target that names it, _self, _top or _parent, and returns it; any
  // other target asks for a new window, which Chrome's pop-up blocker refuses
  // without the user's activation, so it returns null. document.open() with
  // three arguments calls it too.
  Window* Open(const std::u16string& url,
               const std::u16string& target,
               const std::u16string& features,
               bindings::ExceptionState& exception_state);

  // EventTarget:
  Window* ToWindow() override;

  // ExecutionContext:
  bool IsWindow() const override;
  FetchReferrerPolicy GetReferrerPolicy() const override;
  std::u16string Url() const override;
  EventTarget* ErrorEventTarget() override;
  url::Origin GetSecurityOrigin() const override;
  url::Origin AgentClusterOrigin() const override;
  std::string StorageKey() const override;
  GURL CompleteUrl(std::u16string_view url) const override;
  GURL BaseUrl() const override;
  // The timers of geolocation's requests count too.
  size_t TimerCount() const override;

  // What the Page uses.
  mitto::Document& GetDocument() const { return *document_; }
  // Whether it has its document yet, which it has once its context is made
  // and set up.
  bool HasDocument() const { return document_; }
  // LocalDOMWindow::CanExecuteScripts(): whether its document's sandbox lets
  // scripts run. One that is about to run and may not is said to be blocked
  // in the console.
  bool CanExecuteScripts(bool about_to_execute);
  // Makes `document` the window's, and the window its document's.
  void SetDocument(mitto::Document* document);
  // Makes the global proxy and the global object of `context`, which was made
  // from Window's template, this window's wrappers, as Blink's
  // LocalWindowProxy::SetupWindowPrototypeChain() does.
  void AssociateWithContext(v8::Isolate* isolate,
                            v8::Local<v8::Context> context);
  // The context of the global proxy that is the window's wrapper, or empty
  // once the frame's next window has it, so that nothing takes that window's
  // context for this one's. A removed frame's window keeps it.
  v8::MaybeLocal<v8::Context> ContextOfGlobalProxy(v8::Isolate* isolate);
  // The entry of the session history that the window's document is at, as
  // Blink's DocumentLoader holds its HistoryItem: what history.state and
  // scrollRestoration read. None for a window that no Page program loaded,
  // such as the one of a Known Gap.
  const HistoryItem* CurrentHistoryItem() const {
    return current_history_item_.get();
  }
  HistoryItem* MutableCurrentHistoryItem() {
    return current_history_item_.get();
  }
  void SetCurrentHistoryItem(const HistoryItem& item);

  // The window's frame, which the Page sets while the window is its frame's,
  // and clears once another window takes its place.
  void SetFrame(mitto::Frame* frame) { frame_ = frame; }
  mitto::Frame* GetFrame() const { return frame_; }
  // What drops the window's document from the back/forward cache that keeps
  // it, which the Page sets while the cache does: once a script tries to run
  // in its context, as Blink's LocalFrame::EvictFromBackForwardCache() does
  // then.
  void SetBackForwardCacheEviction(base::RepeatingClosure evict) {
    back_forward_cache_eviction_ = std::move(evict);
  }
  void EvictFromBackForwardCache() {
    if (back_forward_cache_eviction_) {
      back_forward_cache_eviction_.Run();
    }
  }
  // Whether its frame is under another.
  bool IsInFrame() const;
  // The process that Chrome has its document in.
  RendererProcess Process() const;
  // What a navigation of the window's frame to another document asks the
  // Session through. Without one, as in the Page program given a script or
  // once the context is gone, such a navigation does not start.
  void SetNavigationClient(NavigationClient* client) {
    navigation_client_ = client;
  }
  NavigationClient* GetNavigationClient() const { return navigation_client_; }
  // The idle callbacks of the window's context, which go with it, as its
  // timers do.
  void SetIdleTasks(IdleTaskController* idle_tasks) { idle_tasks_ = idle_tasks; }
  // How many idle callbacks wait.
  size_t IdleCallbackCount() const;
  // The animation frame callbacks of the window's context, which go with it,
  // as its timers do.
  void SetAnimationFrames(AnimationFrameController* animation_frames) {
    animation_frames_ = animation_frames;
  }
  // How many animation frame callbacks and events wait for a frame.
  size_t AnimationFrameCallbackCount() const;
  // `event` fires at `target` in the next animation frame, before its
  // callbacks, as Blink's Document::EnqueueAnimationFrameEvent() does. Nothing
  // fires in a window with no frames.
  void EnqueueAnimationFrameEvent(EventTarget& target, mitto::Event& event);
  // `steps` runs in the post-lifecycle steps of the next animation frame,
  // and never in a window with no frames.
  void RequestPostLifecycleSteps(base::OnceClosure steps);
  // The window's frames wait until its document is ready to render, as
  // LocalFrameView's lifecycle updates do, from a navigation's commit until
  // Document::BeginLifecycleUpdatesIfRenderingReady().
  void DeferLifecycleUpdates();
  void BeginLifecycleUpdates();
  // How many scheduler.postTask() callbacks and scheduler.yield()
  // continuations wait.
  size_t ScheduledTaskCount() const;
  // The HTML Standard's current event of the window, which a dispatch sets
  // while it calls a listener.
  mitto::Event* CurrentEvent() const { return current_event_.Get(); }
  void SetCurrentEvent(mitto::Event* event) { current_event_ = event; }
  // The ID by which the Page names the window's document to its Session, the
  // context's ID (ExecutionContext::ContextId()).
  void SetDocumentId(uint64_t id) { SetContextId(id); }
  uint64_t DocumentId() const { return ContextId(); }
  // How the window's document loads, which performance.timing and the
  // navigation's entry read. Without one, as in the Page program given a
  // script, the window starts one at its time origin.
  void SetDocumentLoadTiming(DocumentLoadTiming* timing);
  DocumentLoadTiming& GetDocumentLoadTiming();
  // The window's module map and the rest of its module machinery, made the
  // first time, as Blink's Modulator::From() makes the context's.
  Modulator* GetModulator() override;

 private:
  // EventTarget:
  void ListenersChanged(std::u16string_view type) override;

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
  cppgc::Member<Modulator> modulator_;
  cppgc::Member<CustomElementRegistry> custom_elements_;
  cppgc::Member<mitto::Crypto> crypto_;
  cppgc::Member<mitto::Scheduler> scheduler_;
  cppgc::Member<mitto::CookieStore> cookie_store_;
  cppgc::Member<mitto::CacheStorage> caches_;
  cppgc::Member<IDBFactory> indexed_db_;
  cppgc::Member<mitto::Storage> local_storage_;
  cppgc::Member<mitto::Storage> session_storage_;
  cppgc::Member<DocumentLoadTiming> document_load_timing_;
  cppgc::Member<mitto::Event> current_event_;
  raw_ptr<NavigationClient> navigation_client_ = nullptr;
  std::unique_ptr<HistoryItem> current_history_item_;
  raw_ptr<mitto::Frame> frame_ = nullptr;
  base::RepeatingClosure back_forward_cache_eviction_;
  // The name of the Page program's own window, which has no frame to keep it.
  std::u16string frameless_name_;
  raw_ptr<IdleTaskController> idle_tasks_ = nullptr;
  raw_ptr<AnimationFrameController> animation_frames_ = nullptr;
  scoped_refptr<DeferredFetches> deferred_fetches_;
};

}  // namespace mitto

#endif  // MITTO_DOM_WINDOW_H_
