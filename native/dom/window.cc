#include "mitto/dom/window.h"

#include <memory>
#include <tuple>
#include <utility>

#include "base/check.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_window.h"
#include "mitto/dom/abort_signal.h"
#include "mitto/dom/bar_prop.h"
#include "mitto/dom/crash_report_context.h"
#include "mitto/dom/custom_element_registry.h"
#include "mitto/dom/document.h"
#include "mitto/dom/dom_timer.h"
#include "mitto/dom/fetch_loader.h"
#include "mitto/dom/fetcher.h"
#include "mitto/dom/headers.h"
#include "mitto/dom/history.h"
#include "mitto/dom/location.h"
#include "mitto/dom/media_query_list.h"
#include "mitto/dom/navigation.h"
#include "mitto/dom/navigator.h"
#include "mitto/dom/performance.h"
#include "mitto/dom/request.h"
#include "mitto/dom/screen.h"
#include "mitto/dom/style_media.h"
#include "mitto/dom/viewport.h"
#include "mitto/dom/visual_viewport.h"
#include "mitto/header_rules.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-microtask-queue.h"
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
  Window* const window = bindings::V8Window::ToWrappable(
      isolate, isolate->GetCurrentContext()->Global());
  CHECK(window);
  return *window;
}

Window::Window() = default;
Window::~Window() = default;

const bindings::WrapperTypeInfo* Window::GetWrapperTypeInfo() const {
  return &bindings::V8Window::kWrapperTypeInfo;
}

void Window::Trace(cppgc::Visitor* visitor) const {
  WindowProperties::Trace(visitor);
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
  visitor->Trace(custom_elements_);
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
  return base::UTF8ToUTF16(
      url::Origin::Create(GURL(document_->UrlForBinding())).Serialize());
}

bool Window::IsSecureContextForBindings() const {
  return bindings::PerIsolateData::From(v8::Isolate::GetCurrent())
      ->secure_context();
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

mitto::History* Window::History() {
  if (!history_) {
    history_ = History::Create();
  }
  return history_.Get();
}

BarProp* Window::Bar(BarKind kind) {
  if (!bars_[kind]) {
    bars_[kind] = BarProp::Create();
  }
  return bars_[kind].Get();
}

mitto::VisualViewport* Window::VisualViewport() {
  if (!visual_viewport_) {
    visual_viewport_ = VisualViewport::Create(start_);
  }
  return visual_viewport_.Get();
}

mitto::Viewport* Window::Viewport() {
  if (!viewport_) {
    viewport_ = Viewport::Create(start_);
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

CustomElementRegistry* Window::CustomElements() {
  if (!custom_elements_) {
    custom_elements_ = CustomElementRegistry::Create();
  }
  return custom_elements_.Get();
}

mitto::Navigator* Window::Navigator() {
  if (!navigator_) {
    navigator_ = mitto::Navigator::Create(start_, connection_);
  }
  return navigator_.Get();
}

mitto::Screen* Window::Screen() {
  if (!screen_) {
    screen_ = mitto::Screen::Create(start_);
  }
  return screen_.Get();
}

void Window::SetStart(const PageStart& start,
                      const PageConnection& connection) {
  start_ = start;
  connection_ = connection;
}

// As Blink's LocalDOMWindow::matchMedia().
MediaQueryList* Window::MatchMedia(const std::u16string& query) {
  return MediaQueryList::Create(this, query);
}

mitto::Performance* Window::Performance() {
  if (!performance_) {
    performance_ = Performance::Create(
        bindings::PerIsolateData::From(v8::Isolate::GetCurrent())
            ->time_origin());
  }
  return performance_.Get();
}

int32_t Window::SetTimeout(v8::Local<v8::Function> handler,
                           int32_t timeout,
                           const std::vector<v8::Local<v8::Value>>& arguments) {
  return timers_ ? timers_->SetTimeout(handler, timeout, arguments) : 0;
}

int32_t Window::SetTimeout(const std::u16string& handler,
                           int32_t timeout,
                           const std::vector<v8::Local<v8::Value>>& arguments,
                           bindings::ExceptionState& exception_state) {
  // As Chrome, a string handler gets none of the arguments.
  return timers_ ? timers_->SetTimeout(handler, timeout) : 0;
}

void Window::ClearTimeout(int32_t handle) {
  if (timers_) {
    timers_->Clear(handle);
  }
}

int32_t Window::SetInterval(
    v8::Local<v8::Function> handler,
    int32_t timeout,
    const std::vector<v8::Local<v8::Value>>& arguments) {
  return timers_ ? timers_->SetInterval(handler, timeout, arguments) : 0;
}

int32_t Window::SetInterval(const std::u16string& handler,
                            int32_t timeout,
                            const std::vector<v8::Local<v8::Value>>& arguments,
                            bindings::ExceptionState& exception_state) {
  return timers_ ? timers_->SetInterval(handler, timeout) : 0;
}

void Window::ClearInterval(int32_t handle) {
  if (timers_) {
    timers_->Clear(handle);
  }
}

size_t Window::TimerCount() const {
  return timers_ ? timers_->size() : 0;
}

// Upstream: third_party/blink/renderer/core/frame/universal_global_scope.cc
void Window::QueueMicrotask(v8::Local<v8::Function> callback) {
  v8::Isolate::GetCurrent()->EnqueueMicrotask(callback);
}

// Upstream: third_party/blink/renderer/core/fetch/global_fetch.cc
// Upstream: third_party/blink/renderer/core/fetch/fetch_manager.cc
v8::Local<v8::Promise> Window::Fetch(
    const bindings::RequestOrUSVString& input,
    const bindings::RequestInit& init,
    bindings::ExceptionState& exception_state) {
  Request* const request = Request::Create(input, init, exception_state);
  if (!request) {
    return v8::Local<v8::Promise>();
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return v8::Local<v8::Promise>();
  }
  // A request whose signal has aborted rejects with its reason, before any
  // check of where it goes.
  AbortSignal* const signal = request->Signal();
  if (signal->Aborted()) {
    std::ignore = resolver->Reject(context, signal->Reason());
    return resolver->GetPromise();
  }
  const url::Origin document_origin = url::Origin::Create(
      GURL(base::UTF16ToUTF8(GetDocument().UrlForBinding())));
  const bool cross_origin = !document_origin.IsSameOriginWith(request->url());
  // A cross-origin request that needs a CORS preflight is a Known Gap, which
  // the Session refuses too.
  bool needs_preflight = !IsCorsSafelistedMethod(request->Method());
  for (const auto& [name, value] : request->GetHeaders()->list()) {
    needs_preflight |= !IsCorsSafelistedRequestHeader(name, value);
  }
  if (!fetcher_ ||
      (cross_origin &&
       (request->mode() == Request::Mode::kSameOrigin || needs_preflight))) {
    std::ignore = resolver->Reject(
        context, v8::Exception::TypeError(v8::String::NewFromUtf8Literal(
                     isolate, "Failed to fetch")));
    return resolver->GetPromise();
  }
  FetchRequestData data;
  data.method = request->Method();
  data.url = request->url().spec();
  data.headers = request->GetHeaders()->ToLines();
  data.body = request->body();
  data.same_origin = request->mode() == Request::Mode::kSameOrigin;
  fetcher_->Fetch(std::move(data),
                  std::make_unique<FetchLoader>(
                      isolate, context, resolver,
                      /*head=*/request->Method() == "HEAD", signal));
  return resolver->GetPromise();
}

void Window::SetDocument(mitto::Document* document) {
  if (document_) {
    document_->SetWindow(nullptr);
  }
  document_ = document;
  document_->SetWindow(this);
}

void Window::AssociateWithContext(v8::Isolate* isolate,
                                  v8::Local<v8::Context> context) {
  AssociateWithGlobalProxy(isolate, context->Global());
}

}  // namespace mitto
