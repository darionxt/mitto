#include "mitto/dom/navigation.h"

#include <tuple>
#include <utility>

#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_navigation.h"
#include "mitto/dom/abort_controller.h"
#include "mitto/dom/abort_signal.h"
#include "mitto/dom/document.h"
#include "mitto/dom/dom_exception.h"
#include "mitto/dom/error_event.h"
#include "mitto/dom/event.h"
#include "mitto/dom/navigate_event.h"
#include "mitto/dom/navigation_activation.h"
#include "mitto/dom/navigation_client.h"
#include "mitto/dom/navigation_current_entry_change_event.h"
#include "mitto/dom/navigation_destination.h"
#include "mitto/dom/navigation_history_entry.h"
#include "mitto/dom/navigation_transition.h"
#include "mitto/dom/pop_state_event.h"
#include "mitto/dom/resolved_url.h"
#include "mitto/dom/script_values.h"
#include "mitto/dom/window.h"
#include "url/origin.h"
#include "url/url_constants.h"
#include "url/url_util.h"
#include "v8/include/cppgc/garbage-collected.h"
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
  NavigationApiMethodTracker(v8::Local<v8::Value> info,
                             std::optional<SerializedScriptValue> state)
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
  const std::optional<SerializedScriptValue>& state() const { return state_; }

  // The NavigationResult: { committed, finished }.
  v8::Local<v8::Object> Result() const {
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    return PlainObject(isolate,
                       {{"committed", committed_.Get(isolate)->GetPromise()},
                        {"finished", finished_.Get(isolate)->GetPromise()}});
  }

  // The navigation committed to `entry`, which takes the state that
  // navigate() or reload() gave, or none.
  void NotifyAboutTheCommittedToEntry(NavigationHistoryEntry* entry) {
    committed_to_ = entry;
    entry->SetState(std::move(state_));
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
  std::optional<SerializedScriptValue> state_;
  v8::TracedReference<v8::Promise::Resolver> committed_;
  v8::TracedReference<v8::Promise::Resolver> finished_;
  cppgc::Member<NavigationHistoryEntry> committed_to_;
};

namespace {

// Every document gets a sequence number of its own, which its entries share.
uint64_t NextDocumentSequence() {
  static uint64_t last = 0;
  return ++last;
}

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
  }
}

v8::Local<v8::Value> NewDOMException(v8::Isolate* isolate,
                                     std::u16string_view name,
                                     std::u16string_view message) {
  return bindings::ToV8(isolate, DOMException::Create(std::u16string(message),
                                                      std::u16string(name)));
}

}  // namespace

bool CanChangeToUrlForHistoryApi(const GURL& url, const GURL& document_url) {
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
  if (url::Origin::Create(document_url).opaque()) {
    return !differ_in_path;
  }
  if (url.IsStandard()) {
    return true;
  }
  return !differ_in_path && !differ_in_query;
}

Navigation::CarriedEntry::CarriedEntry() = default;
Navigation::CarriedEntry::CarriedEntry(const CarriedEntry&) = default;
Navigation::CarriedEntry& Navigation::CarriedEntry::operator=(
    const CarriedEntry&) = default;
Navigation::CarriedEntry::~CarriedEntry() = default;

Navigation::Carried::Carried() = default;
Navigation::Carried::Carried(const Carried&) = default;
Navigation::Carried& Navigation::Carried::operator=(const Carried&) = default;
Navigation::Carried::~Carried() = default;

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
}

bool Navigation::HasEntriesAndEventsDisabled() const {
  if (!window_ || !window_->HasContext()) {
    return true;
  }
  const Document& document = window_->GetDocument();
  return document.IsInitialEmptyDocument() ||
         url::Origin::Create(GURL(document.UrlForBinding())).opaque();
}

void Navigation::MakeEntries() {
  if (made_) {
    return;
  }
  made_ = true;
  if (HasEntriesAndEventsDisabled()) {
    return;
  }
  document_sequence_ = NextDocumentSequence();
  const std::u16string url = window_->GetDocument().UrlForBinding();
  const Carried carried = carried_.value_or(Carried());
  // The entries before it are the document's only while they are of its
  // origin, as the browser gives a renderer only those.
  const bool same_origin =
      !carried.entries.empty() && carried.current < carried.entries.size() &&
      url::Origin::Create(GURL(carried.url))
          .IsSameOriginWith(url::Origin::Create(GURL(url)));
  const uint64_t carried_sequence = NextDocumentSequence();
  const auto make = [this, carried_sequence](const CarriedEntry& entry) {
    return NavigationHistoryEntry::Create(
        window_, entry.key, entry.id, entry.url, carried_sequence, entry.state);
  };
  NavigationHistoryEntry* from = nullptr;
  if (!same_origin) {
    entries_.emplace_back(NavigationHistoryEntry::Create(
        window_, NavigationHistoryEntry::NewKey(),
        NavigationHistoryEntry::NewKey(), url, document_sequence_,
        std::nullopt));
    current_index_ = 0;
  } else if (carried.navigation_type == u"push") {
    for (size_t i = 0; i <= carried.current; ++i) {
      entries_.emplace_back(make(carried.entries[i]));
    }
    from = entries_.back().Get();
    entries_.emplace_back(NavigationHistoryEntry::Create(
        window_, NavigationHistoryEntry::NewKey(),
        NavigationHistoryEntry::NewKey(), url, document_sequence_,
        std::nullopt));
    current_index_ = static_cast<int64_t>(entries_.size()) - 1;
  } else {
    // A replace keeps the entry's key and gives it a new ID, and a reload
    // keeps both, with the entry's state. The entry it came from is the one
    // of the old entry's key and ID among the entries, as Blink finds it:
    // the new one for a reload, and none of them for a replace.
    const CarriedEntry& old = carried.entries[carried.current];
    for (const CarriedEntry& entry : carried.entries) {
      entries_.emplace_back(make(entry));
    }
    const bool reload = carried.navigation_type == u"reload";
    if (!reload) {
      from = entries_[carried.current].Get();
    }
    entries_[carried.current] = NavigationHistoryEntry::Create(
        window_, old.key, reload ? old.id : NavigationHistoryEntry::NewKey(),
        url, document_sequence_,
        reload ? old.state : std::optional<SerializedScriptValue>());
    current_index_ = static_cast<int64_t>(carried.current);
    if (reload) {
      from = entries_[carried.current].Get();
    }
  }
  activation_ = NavigationActivation::Create(
      entries_[static_cast<size_t>(current_index_)].Get(),
      carried.navigation_type, from);
}

Navigation::Carried Navigation::CarryForNextDocument() {
  MakeEntries();
  Carried carried;
  if (current_index_ < 0) {
    return carried;
  }
  for (const cppgc::Member<NavigationHistoryEntry>& entry : entries_) {
    CarriedEntry item;
    item.key = entry->key();
    item.id = entry->id();
    item.url = entry->url();
    item.state = entry->state();
    carried.entries.push_back(std::move(item));
  }
  carried.current = static_cast<size_t>(current_index_);
  carried.url = window_->GetDocument().UrlForBinding();
  return carried;
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
  std::optional<SerializedScriptValue> state;
  if (options.state && !(*options.state)->IsUndefined()) {
    const v8::TryCatch try_catch(isolate);
    state = SerializedScriptValue::Serialize(isolate, *options.state);
    if (!state) {
      return EarlyErrorResult(try_catch.HasCaught()
                                  ? try_catch.Exception()
                                  : v8::Undefined(isolate).As<v8::Value>());
    }
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
  std::optional<SerializedScriptValue> state;
  if (options.state && !(*options.state)->IsUndefined()) {
    const v8::TryCatch try_catch(isolate);
    state = SerializedScriptValue::Serialize(isolate, *options.state);
    if (!state) {
      return EarlyErrorResult(try_catch.HasCaught()
                                  ? try_catch.Exception()
                                  : v8::Undefined(isolate).As<v8::Value>());
    }
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

Navigation::DispatchResult Navigation::DispatchNavigateEvent(
    const NavigateParams& params) {
  if (HasEntriesAndEventsDisabled()) {
    return DispatchResult::kContinue;
  }
  MakeEntries();
  while (ongoing_navigate_event_) {
    AbortOngoingNavigation();
  }
  ongoing_tracker_ = upcoming_tracker_;
  upcoming_tracker_ = nullptr;
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const GURL document_url(window_->GetDocument().UrlForBinding());
  const std::u16string navigation_type = NavigationTypeOf(params.load_type);
  std::optional<SerializedScriptValue> destination_state;
  if (ongoing_tracker_) {
    destination_state = ongoing_tracker_->state();
  } else if (params.load_type == FrameLoadType::kReload && CurrentEntry()) {
    destination_state = CurrentEntry()->state();
  }
  NavigateEvent::Fields fields;
  fields.navigation_type = navigation_type;
  fields.destination = NavigationDestination::Create(
      base::UTF8ToUTF16(params.url.spec()), params.fragment, destination_state);
  fields.can_intercept = CanChangeToUrlForHistoryApi(params.url, document_url);
  fields.hash_change = params.fragment && params.url != document_url &&
                       EqualIgnoringFragment(params.url, document_url);
  fields.source_element = params.source_element;
  fields.url = params.url;
  fields.fragment = params.fragment;
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

void Navigation::UpdateForNavigation(const GURL& url,
                                     FrameLoadType type,
                                     bool keep_state) {
  if (HasEntriesAndEventsDisabled()) {
    return;
  }
  MakeEntries();
  NavigationHistoryEntry* const old_current = CurrentEntry();
  std::vector<NavigationHistoryEntry*> disposed;
  const std::u16string new_url = base::UTF8ToUTF16(url.spec());
  // A navigation to a fragment keeps the entry's state, and an intercepted
  // one has none, unless navigate() gives it one.
  std::optional<SerializedScriptValue> state =
      keep_state && old_current ? old_current->state() : std::nullopt;
  if (type == FrameLoadType::kStandard) {
    ++current_index_;
    for (size_t i = static_cast<size_t>(current_index_); i < entries_.size();
         ++i) {
      disposed.push_back(entries_[i].Get());
    }
    entries_.resize(static_cast<size_t>(current_index_));
    entries_.emplace_back(NavigationHistoryEntry::Create(
        window_, NavigationHistoryEntry::NewKey(),
        NavigationHistoryEntry::NewKey(), new_url, document_sequence_,
        std::move(state)));
  } else if (type == FrameLoadType::kReplaceCurrentItem) {
    disposed.push_back(old_current);
    entries_[static_cast<size_t>(current_index_)] =
        NavigationHistoryEntry::Create(
            window_, old_current->key(), NavigationHistoryEntry::NewKey(),
            new_url, document_sequence_, std::move(state));
  }
  NavigationHistoryEntry* const current = CurrentEntry();
  if (ongoing_tracker_) {
    ongoing_tracker_->NotifyAboutTheCommittedToEntry(current);
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
    for (NavigationHistoryEntry* const entry : disposed) {
      bindings::EventInit init;
      Event* const dispose = Event::Create(u"dispose", init);
      dispose->SetTrusted(true);
      entry->DispatchEvent(dispose);
    }
  }
  if (ongoing && window_->HasContext()) {
    ongoing->React();
  }
}

void Navigation::RunUrlAndHistoryUpdateSteps(const GURL& url,
                                             FrameLoadType type,
                                             bool fire_popstate) {
  window_->GetDocument().SetUrl(base::UTF8ToUTF16(url.spec()));
  if (NavigationClient* const client = window_->GetNavigationClient()) {
    client->DidNavigateWithinDocument(url);
  }
  UpdateForNavigation(url, type, /*keep_state=*/false);
  if (fire_popstate && window_->HasContext()) {
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    bindings::PopStateEventInit init;
    init.state = v8::Null(isolate);
    PopStateEvent* const popstate = PopStateEvent::Create(u"popstate", init);
    popstate->SetTrusted(true);
    window_->DispatchEvent(popstate);
  }
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
