#include "mitto/dom/navigation.h"

#include <algorithm>
#include <tuple>
#include <utility>

#include "base/functional/function_ref.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_navigation.h"
#include "mitto/dom/abort_controller.h"
#include "mitto/dom/abort_signal.h"
#include "mitto/dom/document.h"
#include "mitto/dom/dom_exception.h"
#include "mitto/dom/error_event.h"
#include "mitto/dom/event.h"
#include "mitto/dom/history_item.h"
#include "mitto/dom/navigate_event.h"
#include "mitto/dom/navigation_activation.h"
#include "mitto/dom/navigation_client.h"
#include "mitto/dom/navigation_current_entry_change_event.h"
#include "mitto/dom/navigation_destination.h"
#include "mitto/dom/navigation_history_entry.h"
#include "mitto/dom/navigation_transition.h"
#include "mitto/dom/resolved_url.h"
#include "mitto/dom/script_values.h"
#include "mitto/dom/window.h"
#include "url/origin.h"
#include "url/url_constants.h"
#include "url/url_util.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-message.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-traced-handle.h"

namespace mitto {

// NavigationApiMethodTracker: what navigate() and reload() asked for, and the
// promises they gave, which the navigation settles. The end's is marked as
// handled, as Blink marks it.
class NavigationApiMethodTracker final
    : public cppgc::GarbageCollected<NavigationApiMethodTracker> {
 public:
  NavigationApiMethodTracker(
      v8::Local<v8::Value> info,
      scoped_refptr<const SharedSerializedScriptValue> state)
      : state_(std::move(state)) {
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();
    info_.Reset(isolate, info);
    const v8::Local<v8::Promise::Resolver> committed =
        v8::Promise::Resolver::New(context).ToLocalChecked();
    const v8::Local<v8::Promise::Resolver> finished =
        v8::Promise::Resolver::New(context).ToLocalChecked();
    finished->GetPromise()->MarkAsHandled();
    committed_.Reset(isolate, committed);
    finished_.Reset(isolate, finished);
  }

  void Trace(cppgc::Visitor* visitor) const {
    visitor->Trace(info_);
    visitor->Trace(committed_);
    visitor->Trace(finished_);
    visitor->Trace(committed_to_);
  }

  v8::Local<v8::Value> Info() const {
    return info_.Get(v8::Isolate::GetCurrent());
  }
  const scoped_refptr<const SharedSerializedScriptValue>& state() const {
    return state_;
  }

  // The NavigationResult: { committed, finished }.
  v8::Local<v8::Object> Result() const {
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    return PlainObject(isolate,
                       {{"committed", committed_.Get(isolate)->GetPromise()},
                        {"finished", finished_.Get(isolate)->GetPromise()}});
  }

  // The navigation committed to `entry`, which takes the state that
  // navigate() or reload() gave, or none, but for a traversal, which keeps
  // the entry's own.
  // NotifyAboutTheCommittedToEntry(), whose SetAndSaveState() `save` does,
  // before the committed promise resolves, as a getter of `then` that a page
  // defined runs then and may move the current entry.
  void NotifyAboutTheCommittedToEntry(NavigationHistoryEntry* entry,
                                      FrameLoadType type,
                                      base::FunctionRef<void()> save) {
    committed_to_ = entry;
    if (type != FrameLoadType::kBackForward) {
      entry->SetState(std::move(state_));
      save();
    }
    state_.reset();
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    std::ignore = committed_.Get(isolate)->Resolve(
        isolate->GetCurrentContext(), bindings::ToV8(isolate, entry));
  }

  void ResolveFinished() {
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    std::ignore = finished_.Get(isolate)->Resolve(
        isolate->GetCurrentContext(),
        committed_to_ ? bindings::ToV8(isolate, committed_to_.Get())
                      : v8::Undefined(isolate).As<v8::Value>());
  }

  // Rejects both promises with `reason`, the commit's only while it waits.
  void RejectFinished(v8::Local<v8::Value> reason) {
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();
    const v8::Local<v8::Promise::Resolver> committed = committed_.Get(isolate);
    if (committed->GetPromise()->State() == v8::Promise::kPending) {
      std::ignore = committed->Reject(context, reason);
    }
    std::ignore = finished_.Get(isolate)->Reject(context, reason);
    state_.reset();
  }

 private:
  v8::TracedReference<v8::Value> info_;
  scoped_refptr<const SharedSerializedScriptValue> state_;
  v8::TracedReference<v8::Promise::Resolver> committed_;
  v8::TracedReference<v8::Promise::Resolver> finished_;
  cppgc::Member<NavigationHistoryEntry> committed_to_;
};

namespace {

bool EqualIgnoringFragment(const GURL& a, const GURL& b) {
  return a.GetWithoutRef() == b.GetWithoutRef();
}

// ToV8NavigationType().
std::u16string NavigationTypeOf(FrameLoadType type) {
  switch (type) {
    case FrameLoadType::kStandard:
      return u"push";
    case FrameLoadType::kReplaceCurrentItem:
      return u"replace";
    case FrameLoadType::kReload:
      return u"reload";
    case FrameLoadType::kBackForward:
      return u"traverse";
  }
}

v8::Local<v8::Value> NewDOMException(v8::Isolate* isolate,
                                     std::u16string_view name,
                                     std::u16string_view message) {
  return bindings::ToV8(isolate, DOMException::Create(std::u16string(message),
                                                      std::u16string(name)));
}

}  // namespace

bool CanChangeToUrlForHistoryApi(const GURL& url,
                                 const url::Origin& document_origin,
                                 const GURL& document_url) {
  if (!url.is_valid()) {
    return false;
  }
  if (url.scheme() != document_url.scheme() ||
      url.username() != document_url.username() ||
      url.password() != document_url.password() ||
      url.host() != document_url.host() || url.port() != document_url.port()) {
    return false;
  }
  if (url.SchemeIsHTTPOrHTTPS()) {
    return true;
  }
  const bool differ_in_path = url.path() != document_url.path();
  const bool differ_in_query = url.query() != document_url.query();
  if (url.SchemeIsFile() && differ_in_path) {
    return false;
  }
  if (document_origin.opaque()) {
    return !differ_in_path;
  }
  if (url.IsStandard()) {
    return true;
  }
  return !differ_in_path && !differ_in_query;
}

Navigation::ApiEntry::ApiEntry() = default;
Navigation::ApiEntry::ApiEntry(const ApiEntry&) = default;
Navigation::ApiEntry& Navigation::ApiEntry::operator=(const ApiEntry&) =
    default;
Navigation::ApiEntry::~ApiEntry() = default;

Navigation::EntryArrays::EntryArrays() = default;
Navigation::EntryArrays::EntryArrays(const EntryArrays&) = default;
Navigation::EntryArrays& Navigation::EntryArrays::operator=(
    const EntryArrays&) = default;
Navigation::EntryArrays::~EntryArrays() = default;

// static
Navigation* Navigation::Create(Window* window) {
  return bindings::MakeGarbageCollected<Navigation>(window);
}

// static
bindings::ScriptWrappable* Navigation::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<Navigation>(
      info, bindings::V8Navigation::kWrapperTypeInfo, nullptr);
}

Navigation::Navigation(Window* window) : window_(window) {}

Navigation::~Navigation() = default;

const bindings::WrapperTypeInfo* Navigation::GetWrapperTypeInfo() const {
  return &bindings::V8Navigation::kWrapperTypeInfo;
}

void Navigation::Trace(cppgc::Visitor* visitor) const {
  EventTarget::Trace(visitor);
  visitor->Trace(window_);
  for (const cppgc::Member<NavigationHistoryEntry>& entry : entries_) {
    visitor->Trace(entry);
  }
  visitor->Trace(activation_);
  visitor->Trace(transition_);
  visitor->Trace(ongoing_navigate_event_);
  visitor->Trace(ongoing_tracker_);
  visitor->Trace(upcoming_tracker_);
  for (const auto& [key, tracker] : upcoming_traverse_trackers_) {
    visitor->Trace(tracker);
  }
}

bool Navigation::HasEntriesAndEventsDisabled() const {
  if (!window_ || !window_->HasContext()) {
    return true;
  }
  // Of the document's origin, not its URL's, which document.open() may have
  // made about:blank, as Blink reads the window's security origin.
  const Document& document = window_->GetDocument();
  return document.IsInitialEmptyDocument() || document.GetOrigin().opaque();
}

int64_t Navigation::DocumentSequence() const {
  const HistoryItem* const item =
      window_ ? window_->CurrentHistoryItem() : nullptr;
  return item ? item->document_sequence_number : 0;
}

void Navigation::MakeEntries() {
  if (made_) {
    return;
  }
  made_ = true;
  if (HasEntriesAndEventsDisabled() || !window_->CurrentHistoryItem()) {
    return;
  }
  const EntryArrays arrays = arrays_.value_or(EntryArrays());
  const auto make = [this](const ApiEntry& entry) {
    return NavigationHistoryEntry::Create(window_, entry.key, entry.id,
                                          entry.url, entry.document_sequence,
                                          entry.state);
  };
  for (const ApiEntry& entry : arrays.back) {
    entries_.emplace_back(make(entry));
  }
  current_index_ = static_cast<int64_t>(entries_.size());
  entries_.emplace_back(MakeEntryFromItem(*window_->CurrentHistoryItem()));
  for (const ApiEntry& entry : arrays.forward) {
    entries_.emplace_back(make(entry));
  }
  // UpdateActivation(): the entry it came from is the one of the previous
  // entry's key and ID among the entries, the new one itself for a reload,
  // and otherwise one of its own, which a replace leaves detached.
  NavigationHistoryEntry* from = nullptr;
  if (arrays.previous) {
    for (const cppgc::Member<NavigationHistoryEntry>& entry : entries_) {
      if (entry->key() == arrays.previous->key &&
          entry->id() == arrays.previous->id) {
        from = entry.Get();
        break;
      }
    }
    if (!from) {
      from = make(*arrays.previous);
    }
  }
  activation_ = NavigationActivation::Create(
      entries_[static_cast<size_t>(current_index_)].Get(),
      NavigationTypeOf(arrays.type), from);
}

NavigationHistoryEntry* Navigation::MakeEntryFromItem(const HistoryItem& item) {
  return NavigationHistoryEntry::Create(
      window_, item.navigation_api_key, item.navigation_api_id,
      base::UTF8ToUTF16(item.url.spec()), item.document_sequence_number,
      item.navigation_api_state);
}

int64_t Navigation::IndexOfKey(const std::u16string& key) const {
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i]->key() == key) {
      return static_cast<int64_t>(i);
    }
  }
  return -1;
}

NavigationHistoryEntry* Navigation::CurrentEntry() {
  MakeEntries();
  if (HasEntriesAndEventsDisabled() || current_index_ < 0) {
    return nullptr;
  }
  return entries_[static_cast<size_t>(current_index_)].Get();
}

NavigationActivation* Navigation::Activation() {
  MakeEntries();
  return activation_.Get();
}

bool Navigation::CanGoBack() {
  MakeEntries();
  return !HasEntriesAndEventsDisabled() && current_index_ > 0;
}

bool Navigation::CanGoForward() {
  MakeEntries();
  return !HasEntriesAndEventsDisabled() && current_index_ >= 0 &&
         static_cast<size_t>(current_index_) + 1 < entries_.size();
}

std::vector<NavigationHistoryEntry*> Navigation::Entries() {
  MakeEntries();
  std::vector<NavigationHistoryEntry*> entries;
  if (HasEntriesAndEventsDisabled()) {
    return entries;
  }
  for (const cppgc::Member<NavigationHistoryEntry>& entry : entries_) {
    entries.push_back(entry.Get());
  }
  return entries;
}

int64_t Navigation::GetIndexFor(const NavigationHistoryEntry* entry) const {
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].Get() == entry) {
      return static_cast<int64_t>(i);
    }
  }
  return -1;
}

v8::Local<v8::Object> Navigation::EarlyErrorResult(v8::Local<v8::Value> error) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const auto rejected = [&] {
    const v8::Local<v8::Promise::Resolver> resolver =
        v8::Promise::Resolver::New(context).ToLocalChecked();
    std::ignore = resolver->Reject(context, error);
    return resolver->GetPromise().As<v8::Value>();
  };
  return PlainObject(isolate,
                     {{"committed", rejected()}, {"finished", rejected()}});
}

bool Navigation::PerformSharedNavigationChecks(std::string_view method,
                                               v8::Local<v8::Value>* error) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (!window_ || !window_->HasContext()) {
    *error = NewDOMException(
        isolate, u"InvalidStateError",
        base::UTF8ToUTF16(base::StrCat(
            {method, "() cannot be called when the Window is detached."})));
    return false;
  }
  if (window_->GetDocument().PageDismissalEventBeingDispatched()) {
    *error = NewDOMException(
        isolate, u"InvalidStateError",
        base::UTF8ToUTF16(base::StrCat(
            {method, "() cannot be called during unload or beforeunload."})));
    return false;
  }
  return true;
}

v8::Local<v8::Object> Navigation::NavigateWithTracker(
    NavigationApiMethodTracker* tracker,
    const FrameLoadRequest& request,
    FrameLoadType type) {
  const v8::Local<v8::Object> result = tracker->Result();
  // A document with no entries navigates, and its promises never settle.
  if (HasEntriesAndEventsDisabled()) {
    mitto::Navigate(*window_, request, type);
    return result;
  }
  upcoming_tracker_ = tracker;
  mitto::Navigate(*window_, request, type);
  // A navigation that dispatched no navigate event did not start.
  if (upcoming_tracker_ == tracker) {
    upcoming_tracker_ = nullptr;
    tracker->RejectFinished(NewDOMException(
        v8::Isolate::GetCurrent(), u"AbortError", u"Navigation was aborted"));
  }
  return result;
}

v8::Local<v8::Object> Navigation::Navigate(
    const std::u16string& url,
    const bindings::NavigationNavigateOptions& options) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  MakeEntries();
  Document& document = window_->GetDocument();
  // KURL(window->BaseURL(), url), with the query in UTF-8.
  const GURL base = document.BaseUrl();
  const GURL completed = base.is_valid() ? base.Resolve(base::UTF16ToUTF8(url))
                                         : GURL(base::UTF16ToUTF8(url));
  if (!completed.is_valid()) {
    const ResolvedURL resolved(
        ResolvedURL(base::UTF8ToUTF16(base.possibly_invalid_spec())), url);
    return EarlyErrorResult(NewDOMException(
        isolate, u"SyntaxError",
        base::StrCat({u"Invalid URL '", resolved.GetString(), u"'."})));
  }
  if (completed.SchemeIs(url::kJavaScriptScheme)) {
    return EarlyErrorResult(
        NewDOMException(isolate, u"NotSupportedError",
                        u"The javascript: protocol is not supported"));
  }
  scoped_refptr<const SharedSerializedScriptValue> state;
  if (options.state && !(*options.state)->IsUndefined()) {
    const v8::TryCatch try_catch(isolate);
    std::optional<SerializedScriptValue> serialized =
        SerializedScriptValue::Serialize(isolate, *options.state);
    if (!serialized) {
      return EarlyErrorResult(try_catch.HasCaught()
                                  ? try_catch.Exception()
                                  : v8::Undefined(isolate).As<v8::Value>());
    }
    state = base::MakeRefCounted<SharedSerializedScriptValue>(
        std::move(*serialized));
  }
  const bool push = options.history == u"push";
  if (push && document.IsInitialEmptyDocument()) {
    return EarlyErrorResult(NewDOMException(
        isolate, u"NotSupportedError",
        u"A \"push\" navigation was explicitly requested, but only a "
        u"\"replace\" navigation is possible while on the initial about:blank "
        u"document."));
  }
  v8::Local<v8::Value> error;
  if (!PerformSharedNavigationChecks("navigate", &error)) {
    return EarlyErrorResult(error);
  }
  auto* const tracker =
      bindings::MakeGarbageCollected<NavigationApiMethodTracker>(
          options.info ? *options.info : v8::Undefined(isolate).As<v8::Value>(),
          std::move(state));
  FrameLoadRequest request;
  request.url = completed;
  request.reason = ClientNavigationReason::kFrameNavigation;
  request.force_history_push = push;
  return NavigateWithTracker(tracker, request,
                             options.history == u"replace"
                                 ? FrameLoadType::kReplaceCurrentItem
                                 : FrameLoadType::kStandard);
}

v8::Local<v8::Object> Navigation::Reload(
    const bindings::NavigationReloadOptions& options) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  MakeEntries();
  scoped_refptr<const SharedSerializedScriptValue> state;
  if (options.state && !(*options.state)->IsUndefined()) {
    const v8::TryCatch try_catch(isolate);
    std::optional<SerializedScriptValue> serialized =
        SerializedScriptValue::Serialize(isolate, *options.state);
    if (!serialized) {
      return EarlyErrorResult(try_catch.HasCaught()
                                  ? try_catch.Exception()
                                  : v8::Undefined(isolate).As<v8::Value>());
    }
    state = base::MakeRefCounted<SharedSerializedScriptValue>(
        std::move(*serialized));
  } else if (NavigationHistoryEntry* const current = CurrentEntry()) {
    state = current->state();
  }
  v8::Local<v8::Value> error;
  if (!PerformSharedNavigationChecks("reload", &error)) {
    return EarlyErrorResult(error);
  }
  auto* const tracker =
      bindings::MakeGarbageCollected<NavigationApiMethodTracker>(
          options.info ? *options.info : v8::Undefined(isolate).As<v8::Value>(),
          std::move(state));
  FrameLoadRequest request;
  request.url = GURL(window_->GetDocument().UrlForBinding());
  request.reason = ClientNavigationReason::kFrameNavigation;
  return NavigateWithTracker(tracker, request, FrameLoadType::kReload);
}

v8::Local<v8::Object> Navigation::EarlySuccessResult(
    NavigationHistoryEntry* entry) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const auto fulfilled = [&] {
    const v8::Local<v8::Promise::Resolver> resolver =
        v8::Promise::Resolver::New(context).ToLocalChecked();
    std::ignore = resolver->Resolve(context, bindings::ToV8(isolate, entry));
    return resolver->GetPromise().As<v8::Value>();
  };
  return PlainObject(isolate,
                     {{"committed", fulfilled()}, {"finished", fulfilled()}});
}

NavigationApiMethodTracker* Navigation::UpcomingTraverseTracker(
    const std::u16string& key) const {
  for (const auto& [tracker_key, tracker] : upcoming_traverse_trackers_) {
    if (tracker_key == key) {
      return tracker.Get();
    }
  }
  return nullptr;
}

v8::Local<v8::Object> Navigation::TraverseTo(
    const std::u16string& key,
    const bindings::NavigationOptions& options) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  MakeEntries();
  v8::Local<v8::Value> error;
  if (!PerformSharedNavigationChecks("traverseTo()/back()/forward", &error)) {
    return EarlyErrorResult(error);
  }
  const int64_t index = IndexOfKey(key);
  if (index == -1) {
    return EarlyErrorResult(
        NewDOMException(isolate, u"InvalidStateError", u"Invalid key"));
  }
  if (index == current_index_) {
    return EarlySuccessResult(CurrentEntry());
  }
  return PerformTraversal(key, options);
}

v8::Local<v8::Object> Navigation::PerformTraversal(
    const std::u16string& key,
    const bindings::NavigationOptions& options) {
  // A traversal to the same entry that waits gives the same promises.
  if (NavigationApiMethodTracker* const upcoming =
          UpcomingTraverseTracker(key)) {
    return upcoming->Result();
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  auto* const tracker =
      bindings::MakeGarbageCollected<NavigationApiMethodTracker>(
          options.info ? *options.info : v8::Undefined(isolate).As<v8::Value>(),
          nullptr);
  upcoming_traverse_trackers_.emplace_back(key, tracker);
  if (NavigationClient* const client = window_->GetNavigationClient()) {
    client->NavigateToNavigationApiKey(key);
  }
  return tracker->Result();
}

v8::Local<v8::Object> Navigation::Back(
    const bindings::NavigationOptions& options) {
  if (!CanGoBack()) {
    return EarlyErrorResult(NewDOMException(
        v8::Isolate::GetCurrent(), u"InvalidStateError", u"Cannot go back"));
  }
  return TraverseTo(entries_[static_cast<size_t>(current_index_) - 1]->key(),
                    options);
}

v8::Local<v8::Object> Navigation::Forward(
    const bindings::NavigationOptions& options) {
  if (!CanGoForward()) {
    return EarlyErrorResult(NewDOMException(
        v8::Isolate::GetCurrent(), u"InvalidStateError", u"Cannot go forward"));
  }
  return TraverseTo(entries_[static_cast<size_t>(current_index_) + 1]->key(),
                    options);
}

void Navigation::UpdateCurrentEntry(
    const bindings::NavigationUpdateCurrentEntryOptions& options,
    bindings::ExceptionState& exception_state) {
  NavigationHistoryEntry* const current = CurrentEntry();
  if (!current) {
    exception_state.ThrowDOMException(
        "InvalidStateError",
        "updateCurrent() cannot be called when navigation.current is null.");
    return;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  std::optional<SerializedScriptValue> serialized;
  {
    const bindings::TryRethrowScope rethrow(isolate, exception_state);
    serialized = SerializedScriptValue::SerializeForStorage(
        isolate,
        options.state.value_or(v8::Undefined(isolate).As<v8::Value>()));
  }
  if (!serialized) {
    return;
  }
  // NavigationHistoryEntry::SetAndSaveState(): the entry takes the state, and
  // the session history keeps it at once, unless a getter that the serializer
  // ran has moved the current entry since.
  const scoped_refptr<const SharedSerializedScriptValue> state =
      base::MakeRefCounted<SharedSerializedScriptValue>(std::move(*serialized));
  current->SetState(state);
  if (current == CurrentEntry()) {
    if (HistoryItem* const item = window_->MutableCurrentHistoryItem()) {
      item->navigation_api_state = state;
      if (NavigationClient* const client = window_->GetNavigationClient()) {
        client->DidUpdateCurrentHistoryItem(*item);
      }
    }
  }
  DispatchEvent(NavigationCurrentEntryChangeEvent::CreateTrusted(u"", current));
}

void Navigation::DisposeEntriesForSessionHistoryRemoval(
    const std::vector<std::u16string>& keys) {
  if (HasEntriesAndEventsDisabled()) {
    return;
  }
  MakeEntries();
  NavigationHistoryEntry* const current = CurrentEntry();
  std::vector<cppgc::Member<NavigationHistoryEntry>> kept;
  // Held until each has had its dispose event, as nothing else may hold
  // them.
  std::vector<cppgc::Persistent<NavigationHistoryEntry>> disposed;
  for (const cppgc::Member<NavigationHistoryEntry>& entry : entries_) {
    if (entry.Get() != current &&
        std::find(keys.begin(), keys.end(), entry->key()) != keys.end()) {
      disposed.push_back(entry.Get());
    } else {
      kept.push_back(entry);
    }
  }
  entries_ = std::move(kept);
  current_index_ = current ? GetIndexFor(current) : -1;
  for (const cppgc::Persistent<NavigationHistoryEntry>& entry : disposed) {
    bindings::EventInit init;
    Event* const dispose = Event::Create(u"dispose", init);
    dispose->SetTrusted(true);
    entry->DispatchEvent(dispose);
  }
}

void Navigation::TraverseCancelled(const std::u16string& key,
                                   TraverseCancelledReason reason) {
  for (auto it = upcoming_traverse_trackers_.begin();
       it != upcoming_traverse_trackers_.end(); ++it) {
    if (it->first != key) {
      continue;
    }
    NavigationApiMethodTracker* const tracker = it->second.Get();
    upcoming_traverse_trackers_.erase(it);
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    tracker->RejectFinished(
        reason == TraverseCancelledReason::kNotFound
            ? NewDOMException(isolate, u"InvalidStateError", u"Invalid key")
            : NewDOMException(isolate, u"AbortError",
                              u"Navigation was aborted"));
    return;
  }
}

Navigation::DispatchResult Navigation::DispatchNavigateEvent(
    const NavigateParams& params) {
  if (HasEntriesAndEventsDisabled()) {
    return DispatchResult::kContinue;
  }
  MakeEntries();
  // A navigation that starts aborts the ongoing one first, whose listeners
  // may change the entries, so a traversal looks for its entry after.
  while (ongoing_navigate_event_) {
    AbortOngoingNavigation();
  }
  if (!window_->HasContext()) {
    return DispatchResult::kAbort;
  }
  const bool traverse = params.load_type == FrameLoadType::kBackForward;
  // A traversal within the document goes to one of the entries, or nowhere.
  // One to another document may go to an entry that the entries leave out,
  // past one of another origin, whose destination then has none.
  NavigationHistoryEntry* destination_entry = nullptr;
  const std::u16string key = traverse && params.destination_item
                                 ? params.destination_item->navigation_api_key
                                 : std::u16string();
  if (traverse) {
    const int64_t index = IndexOfKey(key);
    if (index != -1) {
      destination_entry = entries_[static_cast<size_t>(index)].Get();
    } else if (!params.cross_document) {
      TraverseCancelled(key, TraverseCancelledReason::kAbortedBeforeCommit);
      return DispatchResult::kAbort;
    }
  }
  // PromoteUpcomingNavigationToOngoing(): a traversal's own tracker, by its
  // key, or the navigate() or reload() that waits.
  if (traverse) {
    ongoing_tracker_ = nullptr;
    for (auto it = upcoming_traverse_trackers_.begin();
         it != upcoming_traverse_trackers_.end(); ++it) {
      if (it->first == key) {
        ongoing_tracker_ = it->second;
        upcoming_traverse_trackers_.erase(it);
        break;
      }
    }
  } else {
    ongoing_tracker_ = upcoming_tracker_;
    upcoming_tracker_ = nullptr;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const GURL document_url(window_->GetDocument().UrlForBinding());
  const std::u16string navigation_type = NavigationTypeOf(params.load_type);
  scoped_refptr<const SharedSerializedScriptValue> destination_state;
  if (traverse) {
    destination_state = params.destination_item->navigation_api_state;
  } else if (ongoing_tracker_) {
    destination_state = ongoing_tracker_->state();
  } else if (params.load_type == FrameLoadType::kReload && CurrentEntry()) {
    destination_state = CurrentEntry()->state();
  }
  NavigateEvent::Fields fields;
  fields.navigation_type = navigation_type;
  fields.destination = NavigationDestination::Create(
      base::UTF8ToUTF16(params.url.spec()),
      !params.cross_document && (params.fragment || params.history_api),
      destination_state, destination_entry);
  // A traversal to another document can neither be canceled nor
  // intercepted.
  fields.can_intercept =
      !(traverse && params.cross_document) &&
      CanChangeToUrlForHistoryApi(
          params.url, window_->GetDocument().GetOrigin(), document_url);
  fields.cancelable = !(traverse && params.cross_document);
  fields.hash_change = params.fragment && params.url != document_url &&
                       EqualIgnoringFragment(params.url, document_url);
  fields.source_element = params.source_element;
  fields.url = params.url;
  fields.fragment = params.fragment;
  fields.load_type = params.load_type;
  fields.state_object = params.state_object;
  if (params.destination_item) {
    fields.destination_item = *params.destination_item;
  }
  NavigateEvent* const event = NavigateEvent::Create(
      window_, fields,
      ongoing_tracker_ ? ongoing_tracker_->Info()
                       : v8::Undefined(isolate).As<v8::Value>(),
      AbortController::Create());
  ongoing_navigate_event_ = event;
  has_dropped_navigation_ = false;
  DispatchEvent(event);
  if (event->DefaultPrevented()) {
    if (!event->Signal()->Aborted()) {
      AbortOngoingNavigation();
    }
    return DispatchResult::kAbort;
  }
  if (event->HasNavigationActions()) {
    transition_ = NavigationTransition::Create(navigation_type, CurrentEntry(),
                                               fields.destination);
    event->MaybeCommitImmediately();
    return DispatchResult::kIntercept;
  }
  return DispatchResult::kContinue;
}

void Navigation::UpdateForNavigation(const HistoryItem& item,
                                     FrameLoadType type) {
  if (HasEntriesAndEventsDisabled()) {
    return;
  }
  MakeEntries();
  if (current_index_ < 0) {
    return;
  }
  NavigationHistoryEntry* const old_current = CurrentEntry();
  // Held until each has had its dispose event, as nothing else may hold
  // them.
  std::vector<cppgc::Persistent<NavigationHistoryEntry>> disposed;
  if (type == FrameLoadType::kBackForward) {
    // A traversal moves to the entry of its key, which is among them.
    const int64_t index = IndexOfKey(item.navigation_api_key);
    if (index == -1) {
      return;
    }
    current_index_ = index;
  } else if (type == FrameLoadType::kStandard) {
    ++current_index_;
    for (size_t i = static_cast<size_t>(current_index_); i < entries_.size();
         ++i) {
      disposed.push_back(entries_[i].Get());
    }
    entries_.resize(static_cast<size_t>(current_index_));
    entries_.emplace_back(MakeEntryFromItem(item));
  } else if (type == FrameLoadType::kReplaceCurrentItem) {
    disposed.push_back(old_current);
    entries_[static_cast<size_t>(current_index_)] = MakeEntryFromItem(item);
  }
  NavigationHistoryEntry* const current = CurrentEntry();
  if (ongoing_tracker_) {
    // SetAndSaveState(): the state that navigate() gave is the entry's, which
    // the session history keeps.
    ongoing_tracker_->NotifyAboutTheCommittedToEntry(current, type, [&] {
      HistoryItem* const window_item = window_->MutableCurrentHistoryItem();
      if (!window_item) {
        return;
      }
      window_item->navigation_api_state = current->state();
      if (NavigationClient* const client = window_->GetNavigationClient()) {
        client->DidUpdateCurrentHistoryItem(*window_item);
      }
    });
  }
  if (transition_) {
    transition_->ResolveCommitted();
  }
  NavigateEvent* const ongoing = ongoing_navigate_event_.Get();
  {
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    const v8::MicrotasksScope microtasks(isolate->GetCurrentContext(),
                                         v8::MicrotasksScope::kRunMicrotasks);
    DispatchEvent(NavigationCurrentEntryChangeEvent::CreateTrusted(
        NavigationTypeOf(type), old_current));
    if (ongoing) {
      ongoing->FinalizeNavigationActionPromisesList();
    }
    for (const cppgc::Persistent<NavigationHistoryEntry>& entry : disposed) {
      bindings::EventInit init;
      Event* const dispose = Event::Create(u"dispose", init);
      dispose->SetTrusted(true);
      entry->DispatchEvent(dispose);
    }
    // Inside the scope, as Blink reacts there: a commit that no script ran,
    // such as a traversal's, settles its promises and fires navigatesuccess
    // when the scope ends, before popstate.
    if (ongoing && window_->HasContext()) {
      ongoing->React();
    }
  }
}

void Navigation::RunUrlAndHistoryUpdateSteps(
    const GURL& url,
    const HistoryItem* destination_item,
    scoped_refptr<const SharedSerializedScriptValue> state_object,
    FrameLoadType type,
    bool fire_popstate) {
  UpdateForSameDocumentNavigation(
      *window_, url, destination_item,
      SameDocumentNavigationType::kNavigationApiIntercept,
      std::move(state_object), type, fire_popstate);
}

void Navigation::DidFinishOngoingNavigation() {
  if (ongoing_tracker_) {
    ongoing_tracker_->ResolveFinished();
    ongoing_tracker_ = nullptr;
  }
  bindings::EventInit init;
  Event* const event = Event::Create(u"navigatesuccess", init);
  event->SetTrusted(true);
  DispatchEvent(event);
  if (transition_) {
    transition_->ResolveFinished();
    transition_ = nullptr;
  }
}

void Navigation::DidAbort(v8::Local<v8::Value> reason) {
  if (ongoing_tracker_) {
    ongoing_tracker_->RejectFinished(reason);
    ongoing_tracker_ = nullptr;
  }
  // An ErrorEvent of the reason, whose message is V8's for it, as a console
  // would say it of an exception that nothing caught, and where it says.
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Message> message =
      v8::Exception::CreateMessage(isolate, reason);
  bindings::ErrorEventInit init;
  {
    const v8::TryCatch try_catch(isolate);
    std::ignore =
        bindings::FromV8String(isolate, message->Get(), &init.message);
    if (const v8::Local<v8::Value> name = message->GetScriptResourceName();
        name->IsString()) {
      std::ignore = bindings::FromV8String(isolate, name, &init.filename);
    }
  }
  init.lineno = static_cast<uint32_t>(
      message->GetLineNumber(isolate->GetCurrentContext()).FromMaybe(0));
  init.colno = static_cast<uint32_t>(message->GetStartColumn() + 1);
  init.error = reason;
  ErrorEvent* const event = ErrorEvent::Create(u"navigateerror", init);
  event->SetTrusted(true);
  DispatchEvent(event);
  if (transition_) {
    transition_->RejectFinished(reason);
    transition_ = nullptr;
  }
}

void Navigation::AbortOngoingNavigation() {
  ongoing_navigate_event_->Abort(NewDOMException(
      v8::Isolate::GetCurrent(), u"AbortError", u"Navigation was aborted"));
}

void Navigation::ClearOngoingNavigateEvent(NavigateEvent* event) {
  if (ongoing_navigate_event_ == event) {
    ongoing_navigate_event_ = nullptr;
  }
}

bool Navigation::HasOngoingTracker() const {
  return ongoing_tracker_;
}

}  // namespace mitto
