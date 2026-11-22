#include "mitto/dom/history.h"

#include <utility>

#include "mitto/bindings/v8_history.h"
#include "mitto/dom/document.h"
#include "mitto/dom/frame_loader.h"
#include "mitto/dom/history_item.h"
#include "mitto/dom/navigation_client.h"
#include "mitto/dom/window.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

constexpr std::string_view kNotFullyActive =
    "May not use a History object associated with a Document that is not "
    "fully active";

}  // namespace

// static
History* History::Create(Window* window) {
  return bindings::MakeGarbageCollected<History>(window);
}

// static
bindings::ScriptWrappable* History::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<History>(
      info, bindings::V8History::kWrapperTypeInfo, nullptr);
}

History::History(Window* window) : window_(window) {}
History::~History() = default;

const bindings::WrapperTypeInfo* History::GetWrapperTypeInfo() const {
  return &bindings::V8History::kWrapperTypeInfo;
}

void History::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(window_);
  visitor->Trace(cached_state_);
}

bool History::IsFullyActive() const {
  return window_ && window_->HasContext();
}

uint32_t History::Length(bindings::ExceptionState& exception_state) const {
  if (!IsFullyActive()) {
    exception_state.ThrowDOMException("SecurityError", kNotFullyActive);
    return 0;
  }
  const NavigationClient* const client = window_->GetNavigationClient();
  return client ? static_cast<uint32_t>(client->GetHistoryCounts().length) : 1;
}

std::u16string History::ScrollRestoration(
    bindings::ExceptionState& exception_state) const {
  if (!IsFullyActive()) {
    exception_state.ThrowDOMException("SecurityError", kNotFullyActive);
    return u"auto";
  }
  const HistoryItem* const item = window_->CurrentHistoryItem();
  return item && item->scroll_restoration == ScrollRestorationType::kManual
             ? u"manual"
             : u"auto";
}

void History::SetScrollRestoration(const std::u16string& value,
                                   bindings::ExceptionState& exception_state) {
  HistoryItem* const item =
      IsFullyActive() ? window_->MutableCurrentHistoryItem() : nullptr;
  if (!item) {
    exception_state.ThrowDOMException("SecurityError", kNotFullyActive);
    return;
  }
  const ScrollRestorationType type = value == u"manual"
                                         ? ScrollRestorationType::kManual
                                         : ScrollRestorationType::kAuto;
  if (type == item->scroll_restoration) {
    return;
  }
  item->scroll_restoration = type;
  if (NavigationClient* const client = window_->GetNavigationClient()) {
    client->DidUpdateCurrentHistoryItem(*item);
  }
}

std::shared_ptr<const SerializedScriptValue> History::StateInternal() const {
  if (!IsFullyActive()) {
    return nullptr;
  }
  const HistoryItem* const item = window_->CurrentHistoryItem();
  return item ? item->state : nullptr;
}

bool History::IsSameAsCurrentState(const SerializedScriptValue* state) const {
  return state == StateInternal().get();
}

v8::Local<v8::Value> History::State(bindings::ExceptionState& exception_state) {
  return StateHelper(exception_state);
}

v8::Local<v8::Value> History::StateHelper(
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  // The same object until the entry's state changes, also the null that a
  // document read before it stopped being fully active, as Blink caches it.
  std::shared_ptr<const SerializedScriptValue> current = StateInternal();
  if (last_state_object_requested_ == current && !cached_state_.IsEmpty()) {
    return cached_state_.Get(isolate);
  }
  v8::Local<v8::Value> state;
  if (!IsFullyActive()) {
    exception_state.ThrowDOMException("SecurityError", kNotFullyActive);
    state = v8::Null(isolate);
  } else if (!current) {
    state = v8::Null(isolate);
  } else {
    state = current->Deserialize(isolate);
  }
  last_state_object_requested_ = std::move(current);
  cached_state_.Reset(isolate, state);
  return state;
}

void History::Go(int32_t delta, bindings::ExceptionState& exception_state) {
  if (!IsFullyActive()) {
    exception_state.ThrowDOMException("SecurityError", kNotFullyActive);
    return;
  }
  // FrameNavigationDisabler: nothing navigates while beforeunload, pagehide
  // or unload is dispatched.
  if (window_->GetDocument().PageDismissalEventBeingDispatched()) {
    return;
  }
  if (!CanProceedWithNavigation(*window_)) {
    return;
  }
  if (delta == 0) {
    // LocalFrame::Reload(): go(0) reloads the document, as location.reload()
    // does.
    FrameLoadRequest request;
    request.url = GURL(window_->GetDocument().UrlForBinding());
    request.reason = ClientNavigationReason::kReload;
    StartNavigation(*window_, request, FrameLoadType::kReload);
    return;
  }
  NavigationClient* const client = window_->GetNavigationClient();
  if (!client) {
    return;
  }
  // LocalFrameClientImpl::NavigateBackForward(): an offset past the entries
  // that the frame knows of does nothing.
  const HistoryCounts counts = client->GetHistoryCounts();
  const int back = counts.index > 0 ? counts.index : 0;
  const int forward = counts.length - back - 1;
  if (delta > forward || delta < -back) {
    return;
  }
  client->NavigateBackForward(delta);
}

void History::Back(bindings::ExceptionState& exception_state) {
  Go(-1, exception_state);
}

void History::Forward(bindings::ExceptionState& exception_state) {
  Go(1, exception_state);
}

void History::PushState(v8::Local<v8::Value> data,
                        const std::u16string& title,
                        const std::optional<std::u16string>& url,
                        bindings::ExceptionState& exception_state) {
  StateObjectAdded(data, url, /*replace=*/false, exception_state);
}

void History::ReplaceState(v8::Local<v8::Value> data,
                           const std::u16string& title,
                           const std::optional<std::u16string>& url,
                           bindings::ExceptionState& exception_state) {
  StateObjectAdded(data, url, /*replace=*/true, exception_state);
}

void History::StateObjectAdded(v8::Local<v8::Value> data,
                               const std::optional<std::u16string>& url,
                               bool replace,
                               bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  // The state is serialized first, as Blink serializes it for storage, so
  // that its DataCloneError comes before any other.
  std::optional<SerializedScriptValue> serialized;
  {
    const bindings::TryRethrowScope rethrow(isolate, exception_state);
    serialized = SerializedScriptValue::SerializeForStorage(isolate, data);
  }
  if (!serialized) {
    return;
  }
  if (!IsFullyActive()) {
    exception_state.ThrowDOMException("SecurityError", kNotFullyActive);
    return;
  }
  AddStateObject(
      *window_,
      std::make_shared<const SerializedScriptValue>(std::move(*serialized)),
      url,
      replace ? FrameLoadType::kReplaceCurrentItem : FrameLoadType::kStandard,
      exception_state);
}

}  // namespace mitto
