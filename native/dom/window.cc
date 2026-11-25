#include "mitto/dom/window.h"

#include <stdint.h>

#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_window.h"
#include "mitto/dom/abort_signal.h"
#include "mitto/dom/animation_frame_controller.h"
#include "mitto/dom/bar_prop.h"
#include "mitto/dom/cache_storage.h"
#include "mitto/dom/cookie_store.h"
#include "mitto/dom/crash_report_context.h"
#include "mitto/dom/crypto.h"
#include "mitto/dom/custom_element_registry.h"
#include "mitto/dom/document.h"
#include "mitto/dom/document_load_timing.h"
#include "mitto/dom/document_named_properties.h"
#include "mitto/dom/dom_file_system.h"
#include "mitto/dom/event.h"
#include "mitto/dom/fetcher.h"
#include "mitto/dom/frame.h"
#include "mitto/dom/headers.h"
#include "mitto/dom/history.h"
#include "mitto/dom/history_item.h"
#include "mitto/dom/html_frame_owner_element.h"
#include "mitto/dom/idb_factory.h"
#include "mitto/dom/idle_task_controller.h"
#include "mitto/dom/location.h"
#include "mitto/dom/media_query_list.h"
#include "mitto/dom/modulator.h"
#include "mitto/dom/navigation.h"
#include "mitto/dom/navigator.h"
#include "mitto/dom/performance.h"
#include "mitto/dom/request.h"
#include "mitto/dom/scheduler.h"
#include "mitto/dom/screen.h"
#include "mitto/dom/script_values.h"
#include "mitto/dom/storage.h"
#include "mitto/dom/style_media.h"
#include "mitto/dom/viewport.h"
#include "mitto/dom/visual_viewport.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-promise.h"

namespace mitto {

// static
Window* Window::Create() {
  return bindings::MakeGarbageCollected<Window>();
}

// static
Window& Window::Current() {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  Window* const window = From(isolate, isolate->GetCurrentContext());
  CHECK(window);
  return *window;
}

// static
Window* Window::From(v8::Isolate* isolate, v8::Local<v8::Context> context) {
  ExecutionContext* const execution_context =
      ExecutionContext::From(isolate, context);
  return execution_context && execution_context->IsWindow()
             ? static_cast<Window*>(execution_context)
             : nullptr;
}

// static
Window* Window::Entered() {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (!isolate->InContext()) {
    return nullptr;
  }
  return From(isolate, isolate->GetEnteredOrMicrotaskContext());
}

// static
Window* Window::Incumbent() {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (!isolate->InContext()) {
    return nullptr;
  }
  return From(isolate, isolate->GetIncumbentContext());
}

// static
bool Window::AccessCheckCallback(v8::Local<v8::Context> accessing_context,
                                 v8::Local<v8::Object> accessed_object,
                                 v8::Local<v8::Value> data) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  Window* const accessing = From(isolate, accessing_context);
  Window* const accessed =
      bindings::V8Window::ToWrappable(isolate, accessed_object);
  return accessing && accessing->HasDocument() && accessed &&
         accessed->HasDocument() &&
         accessing->GetSecurityOrigin().IsSameOriginWith(
             accessed->GetSecurityOrigin());
}

Window::Window() = default;
Window::~Window() = default;

const bindings::WrapperTypeInfo* Window::GetWrapperTypeInfo() const {
  return &bindings::V8Window::kWrapperTypeInfo;
}

void Window::Trace(cppgc::Visitor* visitor) const {
  WindowProperties::Trace(visitor);
  ExecutionContext::Trace(visitor);
  visitor->Trace(document_);
  visitor->Trace(performance_);
  visitor->Trace(navigator_);
  visitor->Trace(screen_);
  visitor->Trace(crash_report_);
  visitor->Trace(location_);
  visitor->Trace(history_);
  for (const cppgc::Member<BarProp>& bar : bars_) {
    visitor->Trace(bar);
  }
  visitor->Trace(visual_viewport_);
  visitor->Trace(viewport_);
  visitor->Trace(style_media_);
  visitor->Trace(navigation_);
  visitor->Trace(modulator_);
  visitor->Trace(custom_elements_);
  visitor->Trace(crypto_);
  visitor->Trace(scheduler_);
  visitor->Trace(cookie_store_);
  visitor->Trace(caches_);
  visitor->Trace(indexed_db_);
  visitor->Trace(local_storage_);
  visitor->Trace(session_storage_);
  visitor->Trace(current_event_);
  visitor->Trace(document_load_timing_);
}

// WindowIdleTasks::requestIdleCallback(): 0 once the context has gone.
uint32_t Window::RequestIdleCallback(
    v8::Local<v8::Function> callback,
    const bindings::IdleRequestOptions& options) {
  return idle_tasks_ ? idle_tasks_->RequestIdleCallback(callback, options.timeout)
                     : 0;
}

void Window::CancelIdleCallback(uint32_t handle) {
  if (idle_tasks_) {
    idle_tasks_->CancelIdleCallback(handle);
  }
}

size_t Window::IdleCallbackCount() const {
  return idle_tasks_ ? idle_tasks_->size() : 0;
}

// ScriptedAnimationController::RegisterFrameCallback(): 0 once the context has
// gone.
int32_t Window::RequestAnimationFrame(v8::Local<v8::Function> callback) {
  return animation_frames_
             ? animation_frames_->RequestAnimationFrame(callback,
                                                        /*legacy=*/false)
             : 0;
}

int32_t Window::WebkitRequestAnimationFrame(v8::Local<v8::Function> callback) {
  return animation_frames_
             ? animation_frames_->RequestAnimationFrame(callback,
                                                        /*legacy=*/true)
             : 0;
}

void Window::CancelAnimationFrame(int32_t handle) {
  if (animation_frames_) {
    animation_frames_->CancelAnimationFrame(handle);
  }
}

void Window::RequestPostLifecycleSteps(base::OnceClosure steps) {
  if (animation_frames_) {
    animation_frames_->RequestPostLifecycleSteps(std::move(steps));
  }
}

size_t Window::AnimationFrameCallbackCount() const {
  return animation_frames_ ? animation_frames_->size() : 0;
}

void Window::EnqueueAnimationFrameEvent(EventTarget& target,
                                        mitto::Event& event) {
  if (animation_frames_) {
    animation_frames_->EnqueueEvent(target, event);
  }
}

void Window::DeferLifecycleUpdates() {
  if (animation_frames_) {
    animation_frames_->DeferLifecycleUpdates();
  }
}

void Window::BeginLifecycleUpdates() {
  if (animation_frames_) {
    animation_frames_->BeginLifecycleUpdates();
  }
}

size_t Window::ScheduledTaskCount() const {
  return scheduler_ ? scheduler_->pending_tasks() : 0;
}

// LocalDOMWindow::event(): the current event, or undefined.
v8::Local<v8::Value> Window::Event() const {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (!current_event_) {
    return v8::Undefined(isolate);
  }
  return current_event_->ToV8(isolate);
}

uint32_t Window::Length() const {
  return frame_ ? static_cast<uint32_t>(frame_->ChildCount()) : 0;
}

// DOMWindow::top(): the window of the frame's top-level frame. A window without
// a frame has none, unless it is the Page program's own, which runs a script
// with no frames and is a top-level window.
Window* Window::Top() {
  if (!frame_) {
    return HasContext() ? this : nullptr;
  }
  return frame_->Top().DomWindow();
}

// DOMWindow::parent(): the window of the frame's parent, or the frame's own
// for a top-level frame.
Window* Window::Parent() {
  if (!frame_) {
    return HasContext() ? this : nullptr;
  }
  mitto::Frame* const parent = frame_->Parent();
  return parent ? parent->DomWindow() : frame_->DomWindow();
}

Window* Window::AnonymousIndexedGetter(uint32_t index) {
  if (!frame_ || index >= frame_->ChildCount()) {
    return nullptr;
  }
  return frame_->ChildAt(index)->DomWindow();
}

Element* Window::FrameElement() const {
  HTMLFrameOwnerElement* const owner = frame_ ? frame_->Owner() : nullptr;
  if (!owner) {
    return nullptr;
  }
  // [CheckSecurity=ReturnValue]: the element only for a caller of its
  // document's origin.
  if (!Window::Current().GetSecurityOrigin().IsSameOriginWith(
          owner->GetDocument().GetOrigin())) {
    return nullptr;
  }
  return owner;
}

std::u16string Window::Name() const {
  if (frame_) {
    return frame_->Name();
  }
  return HasContext() ? frameless_name_ : std::u16string();
}

void Window::SetName(const std::u16string& name) {
  if (frame_) {
    frame_->SetName(name);
  } else if (HasContext()) {
    // The Page program's own window, a top-level one with no frame, keeps it.
    frameless_name_ = name;
  }
}

v8::Local<v8::Value> Window::OpenerForBindings() const {
  return v8::Null(v8::Isolate::GetCurrent());
}

// DOMWindow::setOpenerForBindings(): null disowns the opener, which a window
// that a Session opened has not, and any other value shadows the attribute
// with a data property of the window.
void Window::SetOpenerForBindings(v8::Local<v8::Value> opener,
                                  bindings::ExceptionState& exception_state) {
  if (opener->IsNull()) {
    return;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  v8::PropertyDescriptor descriptor(opener, /*writable=*/true);
  descriptor.set_enumerable(true);
  descriptor.set_configurable(true);
  std::ignore = ToV8(isolate)->DefineProperty(
      isolate->GetCurrentContext(),
      v8::String::NewFromUtf8Literal(isolate, "opener"), descriptor);
}

// The serialized origin of the window's document, "null" for an opaque one, as
// LocalDOMWindow::origin() gives it.
std::u16string Window::Origin() const {
  return base::UTF8ToUTF16(document_->GetOrigin().Serialize());
}

bool Window::IsSecureContextForBindings() const {
  return IsSecureContext();
}

CrashReportContext* Window::CrashReport() {
  if (!crash_report_) {
    crash_report_ = CrashReportContext::Create();
  }
  return crash_report_.Get();
}

mitto::Location* Window::Location() {
  if (!location_) {
    location_ = Location::Create(this);
  }
  return location_.Get();
}

void Window::SetCurrentHistoryItem(const HistoryItem& item) {
  current_history_item_ = std::make_unique<HistoryItem>(item);
}

mitto::History* Window::History() {
  if (!history_) {
    history_ = History::Create(this);
  }
  return history_.Get();
}

BarProp* Window::Bar(BarKind kind) {
  if (!bars_[kind]) {
    bars_[kind] = BarProp::Create(*this);
  }
  return bars_[kind].Get();
}

mitto::VisualViewport* Window::VisualViewport() {
  if (!visual_viewport_) {
    visual_viewport_ = VisualViewport::Create(Start());
  }
  return visual_viewport_.Get();
}

mitto::Viewport* Window::Viewport() {
  if (!viewport_) {
    viewport_ = Viewport::Create(Start());
  }
  return viewport_.Get();
}

mitto::StyleMedia* Window::StyleMedia() {
  if (!style_media_) {
    style_media_ = StyleMedia::Create();
  }
  return style_media_.Get();
}

mitto::Navigation* Window::Navigation() {
  if (!navigation_) {
    navigation_ = Navigation::Create(this);
  }
  return navigation_.Get();
}

Modulator* Window::GetModulator() {
  if (!modulator_) {
    modulator_ = bindings::MakeGarbageCollected<Modulator>(*this);
  }
  return modulator_.Get();
}

mitto::Crypto* Window::Crypto() {
  if (!crypto_) {
    crypto_ = mitto::Crypto::Create();
  }
  return crypto_.Get();
}

// GlobalCookieStore::cookieStore(): the window's supplement.
mitto::CookieStore* Window::CookieStore() {
  if (!cookie_store_) {
    cookie_store_ = bindings::MakeGarbageCollected<mitto::CookieStore>(this);
  }
  return cookie_store_.Get();
}

mitto::CacheStorage* Window::Caches(
    bindings::ExceptionState& exception_state) {
  // GlobalCacheStorage::CanCreateCacheStorage(): a document whose origin is
  // opaque, as one that the Session did not load over HTTP or HTTPS has, may
  // not.
  if (!GetDocument().GetCacheStorageHost()) {
    exception_state.ThrowDOMException("SecurityError",
                                      "Access to cache storage is denied.");
    return nullptr;
  }
  if (!caches_) {
    caches_ = bindings::MakeGarbageCollected<mitto::CacheStorage>(this);
  }
  return caches_.Get();
}

IDBFactory* Window::IndexedDB() {
  if (!indexed_db_) {
    indexed_db_ = bindings::MakeGarbageCollected<IDBFactory>(this);
  }
  return indexed_db_.Get();
}

// DOMScheduler::scheduler(): the window's supplement.
mitto::Scheduler* Window::Scheduler() {
  if (!scheduler_) {
    scheduler_ = bindings::MakeGarbageCollected<mitto::Scheduler>(this);
  }
  return scheduler_.Get();
}

CustomElementRegistry* Window::CustomElements() {
  if (!custom_elements_) {
    custom_elements_ = CustomElementRegistry::Create();
  }
  return custom_elements_.Get();
}

mitto::Navigator* Window::Navigator() {
  if (!navigator_) {
    navigator_ = mitto::Navigator::Create(Start(), Connection());
  }
  return navigator_.Get();
}

mitto::Screen* Window::Screen() {
  if (!screen_) {
    screen_ = mitto::Screen::Create(Start());
  }
  return screen_.Get();
}

// As Blink's LocalDOMWindow::matchMedia().
MediaQueryList* Window::MatchMedia(const std::u16string& query) {
  return MediaQueryList::Create(this, query);
}

CacheStorageHost* Window::GetCacheStorageHost() const {
  return GetDocument().GetCacheStorageHost();
}

IndexedDbHost* Window::GetIndexedDbHost() const {
  return GetDocument().GetIndexedDbHost();
}

StorageBucketHost* Window::GetStorageBucketHost() const {
  return GetDocument().GetStorageBucketHost();
}

bool Window::HasPublicKeyRequest() const {
  return navigator_ && navigator_->HasPublicKeyRequest();
}

mitto::Performance* Window::Performance() {
  if (!performance_) {
    performance_ = Performance::Create(TimeOrigin(), &GetDocumentLoadTiming(),
                                       CrossOriginIsolatedCapability());
  }
  return performance_.Get();
}

void Window::SetDocumentLoadTiming(DocumentLoadTiming* timing) {
  document_load_timing_ = timing;
  if (timing) {
    timing->cross_origin_isolated_capability = CrossOriginIsolatedCapability();
  }
}

DocumentLoadTiming& Window::GetDocumentLoadTiming() {
  if (!document_load_timing_) {
    document_load_timing_ =
        bindings::MakeGarbageCollected<DocumentLoadTiming>(TimeOrigin());
  }
  return *document_load_timing_;
}

Window* Window::ToWindow() {
  return this;
}

bool Window::IsWindow() const {
  return true;
}

std::u16string Window::Url() const {
  return GetDocument().UrlForBinding();
}

EventTarget* Window::ErrorEventTarget() {
  return this;
}

url::Origin Window::GetSecurityOrigin() const {
  return GetDocument().GetOrigin();
}

url::Origin Window::AgentClusterOrigin() const {
  return GetSecurityOrigin();
}

GURL Window::CompleteUrl(std::u16string_view url) const {
  return GetDocument().CompleteUrl(url);
}

GURL Window::BaseUrl() const {
  return GetDocument().BaseUrl();
}

size_t Window::TimerCount() const {
  return ExecutionContext::TimerCount() +
         (navigator_ ? navigator_->GeolocationTimerCount() : 0);
}

// Upstream: third_party/blink/renderer/modules/filesystem/dom_window_file_system.cc
// Upstream: third_party/blink/renderer/modules/filesystem/file_system_callbacks.cc
// Upstream: third_party/blink/renderer/core/fileapi/file_error.cc
void Window::WebkitRequestFileSystem(uint16_t type,
                                     int64_t size,
                                     v8::Local<v8::Object> success_callback,
                                     v8::Local<v8::Object> error_callback) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  // DOMFileSystem::ReportError(): the error callback, if any, from a task of
  // its own, with the DOMException of FileError's code and message.
  const auto report_error = [&](std::string name, std::string message) {
    if (error_callback.IsEmpty()) {
      return;
    }
    CallCallbackInterfaceFromTask(
        isolate, error_callback, "ErrorCallback",
        base::BindOnce(
            [](const std::string& name, const std::string& message,
               v8::Isolate* isolate) {
              return bindings::PerIsolateData::From(isolate)
                  ->dom_exception_hooks()
                  .create(isolate, name, message);
            },
            std::move(name), std::move(message)));
  };
  const url::Origin origin = url::Origin::Create(
      GURL(base::UTF16ToUTF8(GetDocument().UrlForBinding())));
  // SecurityOrigin::CanAccessFileSystem().
  if (origin.opaque()) {
    report_error("SecurityError",
                 "It was determined that certain files are unsafe for access "
                 "within a Web application, or that too many calls are being "
                 "made on file resources.");
    return;
  }
  if (type != static_cast<uint16_t>(DOMFileSystem::Type::kTemporary) &&
      type != static_cast<uint16_t>(DOMFileSystem::Type::kPersistent)) {
    report_error("InvalidModificationError",
                 "The object can not be modified in this way.");
    return;
  }
  // The browser opens the origin's file system whatever size the page asks
  // for, and FileSystemCallbacks makes its DOMFileSystem.
  CallCallbackInterfaceFromTask(
      isolate, success_callback, "FileSystemCallback",
      base::BindOnce(
          [](const url::Origin& origin, DOMFileSystem::Type type,
             v8::Isolate* isolate) -> v8::Local<v8::Value> {
            return DOMFileSystem::Create(origin, type)->ToV8(isolate);
          },
          origin, static_cast<DOMFileSystem::Type>(type)));
}

void Window::SetDocument(mitto::Document* document) {
  if (document_) {
    document_->SetWindow(nullptr);
  }
  document_ = document;
  document_->SetWindow(this);
  // LocalWindowProxy::UpdateDocumentWrapper(): the document's wrapper is
  // made in the window's context once it is the window's, so that it is of
  // the window's realm whichever context reaches it first.
  if (HasWrapper()) {
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    v8::Local<v8::Context> context;
    if (ToV8(isolate)->GetCreationContext(isolate).ToLocal(&context)) {
      const v8::Context::Scope context_scope(context);
      std::ignore = document_->ToV8(isolate);
    }
  }
  // A document that elements gave named items before it was the window's,
  // such as the one the parser makes of a Page's markup, has their
  // properties now.
  AddNamedItemProperties(*document_);
}

v8::MaybeLocal<v8::Context> Window::ContextOfGlobalProxy(v8::Isolate* isolate) {
  v8::Local<v8::Context> context;
  if (!ToV8(isolate)->GetCreationContext(isolate).ToLocal(&context) ||
      From(isolate, context) != this) {
    return v8::MaybeLocal<v8::Context>();
  }
  return context;
}

void Window::AssociateWithContext(v8::Isolate* isolate,
                                  v8::Local<v8::Context> context) {
  AssociateWithGlobalProxy(isolate, context->Global());
}

}  // namespace mitto
