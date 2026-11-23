#include "mitto/dom/frame_loader.h"

#include <stddef.h>

#include <string>
#include <string_view>
#include <tuple>

#include "base/check.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/dom/document.h"
#include "mitto/dom/error_reporting.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/frame.h"
#include "mitto/dom/hash_change_event.h"
#include "mitto/dom/history.h"
#include "mitto/dom/history_item.h"
#include "mitto/dom/navigation.h"
#include "mitto/dom/navigation_client.h"
#include "mitto/dom/navigation_rate_limiter.h"
#include "mitto/dom/pop_state_event.h"
#include "mitto/dom/resolved_url.h"
#include "mitto/dom/window.h"
#include "mitto/dom/window_tasks.h"
#include "mitto/mime_types.h"
#include "url/url_canon.h"
#include "url/url_constants.h"
#include "url/url_util.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-script.h"

namespace mitto {
namespace {

// The rate limiter of `window`'s frame, or of the Page program's own window,
// which has no frame.
NavigationRateLimiter& RateLimiterOf(Window& window) {
  if (Frame* const frame = window.GetFrame()) {
    return frame->GetNavigationRateLimiter();
  }
  static base::NoDestructor<NavigationRateLimiter> frameless;
  return *frameless;
}

// Whether `a` and `b` are the same but for their fragments.
bool EqualIgnoringFragment(const GURL& a, const GURL& b) {
  return a.GetWithoutRef() == b.GetWithoutRef();
}

// KURL::ElidedString(): a URL longer than 1024 characters keeps its first
// 511 and its last 510, with "..." between them.
std::string Elided(std::string_view spec) {
  if (spec.size() <= 1024) {
    return std::string(spec);
  }
  return base::StrCat(
      {spec.substr(0, 511), "...", spec.substr(spec.size() - 510)});
}

std::string Elided(const GURL& url) {
  return Elided(url.possibly_invalid_spec());
}

// network_utils::IsDataURLMimeTypeSupported(): whether Chrome would show what
// a data: URL holds, by the MIME type before its first comma, text/plain for
// none, as net::DataURL::Parse() reads it. One without a comma is no data:
// URL at all.
bool IsDataUrlMimeTypeSupported(const GURL& url) {
  const std::string_view content = url.GetContentPiece();
  const size_t comma = content.find(',');
  if (comma == std::string_view::npos) {
    return false;
  }
  std::string_view mime_type = content.substr(0, comma);
  mime_type = mime_type.substr(0, mime_type.find(';'));
  mime_type = base::TrimWhitespaceASCII(mime_type, base::TRIM_ALL);
  return IsSupportedMimeType(mime_type.empty() ? "text/plain" : mime_type);
}

// LocalFrame::NavigationShouldReplaceCurrentHistoryEntry(): a navigation to
// the document's own URL, and one that a script starts while the document
// loads without the user's activation, but for a link and window.open(),
// replace the current entry. A Page has no user activation yet.
bool ShouldReplaceCurrentEntry(Window& window,
                               const FrameLoadRequest& request) {
  Document& document = window.GetDocument();
  if (request.url == GURL(document.UrlForBinding())) {
    return true;
  }
  return !document.LoadEventFinished() &&
         request.reason != ClientNavigationReason::kNone &&
         request.reason != ClientNavigationReason::kAnchorClick;
}

// Fires hashchange at the current window, from its task.
void FireHashChange(std::u16string old_url, std::u16string new_url) {
  Window::Current().DispatchEvent(
      HashChangeEvent::CreateTrusted(old_url, new_url));
}

// Queues hashchange at the window, from a task of the DOM manipulation task
// source, if `url` differs from `old_url` only in its fragment, as Blink's
// CommitSameDocumentNavigationInternal() enqueues it before the commit.
void MaybeEnqueueHashChange(Window& window,
                            const GURL& url,
                            const GURL& old_url) {
  const bool hash_change =
      EqualIgnoringFragment(url, old_url) &&
      (url.has_ref() != old_url.has_ref() || url.ref() != old_url.ref());
  if (hash_change && window.GetTasks()) {
    window.GetTasks()->PostTask(
        TaskType::kDOMManipulation,
        base::BindOnce(&FireHashChange, base::UTF8ToUTF16(old_url.spec()),
                       base::UTF8ToUTF16(url.spec())));
  }
}

// DocumentLoader::CommitSameDocumentNavigation() for a navigation to a
// fragment: the document takes `url` at once, a new entry or the current one
// replaced, the Session hears of it, popstate fires at the window with a null
// state, and hashchange from a task of the DOM manipulation task source if the
// fragment changed.
void CommitSameDocumentNavigation(Window& window,
                                  const FrameLoadRequest& request,
                                  FrameLoadType type) {
  const GURL& url = request.url;
  // The navigate event first, which may cancel it, or intercept it, which
  // commits it already.
  Navigation::NavigateParams params;
  params.url = url;
  params.fragment = true;
  params.load_type = type;
  params.source_element = request.source_element;
  if (window.Navigation()->DispatchNavigateEvent(params) !=
      Navigation::DispatchResult::kContinue) {
    return;
  }
  Document& document = window.GetDocument();
  MaybeEnqueueHashChange(window, url, GURL(document.UrlForBinding()));
  UpdateForSameDocumentNavigation(window, url, /*history_item=*/nullptr,
                                  SameDocumentNavigationType::kFragment,
                                  /*state=*/nullptr, type,
                                  /*fire_popstate=*/true);
  if (!window.HasContext()) {
    return;
  }
  // FrameLoader::ProcessScrollForSameDocumentNavigation(), after the events
  // of the commit: this navigation's fragment indicates the document's
  // target, also when a listener has navigated on meanwhile, as Blink takes
  // the URL that it commits.
  document.ProcessUrlFragment(url);
}

// Runs the script of a javascript: URL in the current window, from its task,
// as Blink's ScriptController::ExecuteJavaScriptURL() does: the URL's text
// after the scheme, its escapes decoded as UTF-8, run as a classic script.
// A string that it gives would take the document's place: a Known Gap.
void ExecuteJavaScriptUrl(std::u16string source) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::HandleScope handle_scope(isolate);
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::MicrotasksScope microtasks_scope(
      context, v8::MicrotasksScope::kRunMicrotasks);
  const v8::TryCatch try_catch(isolate);
  v8::Local<v8::String> code;
  v8::Local<v8::Script> script;
  // ClassicScript::CreateUnspecifiedScript(): no URL of its own, and its
  // errors, a SyntaxError among them, go to the window as they are.
  v8::ScriptOrigin origin = ClassicScriptOrigin(v8::String::Empty(isolate),
                                                /*muted_errors=*/false, 0, 0);
  if (source.size() <= static_cast<size_t>(v8::String::kMaxLength) &&
      v8::String::NewFromTwoByte(
          isolate, reinterpret_cast<const uint16_t*>(source.data()),
          v8::NewStringType::kNormal, base::checked_cast<int>(source.size()))
          .ToLocal(&code) &&
      v8::Script::Compile(context, code, &origin).ToLocal(&script)) {
    std::ignore = script->Run(context);
  }
  ReportScriptException(isolate, try_catch);
}

// Document::ProcessJavaScriptUrl(): the script of `url` runs from a task of
// the networking task source.
void ProcessJavaScriptUrl(Window& window, const GURL& url) {
  if (!window.GetTasks()) {
    return;
  }
  const std::string_view spec = url.spec();
  const std::string_view after_scheme =
      spec.substr(std::string_view(url::kJavaScriptScheme).size() + 1);
  url::RawCanonOutputW<1024> decoded;
  url::DecodeUrlEscapeSequences(
      after_scheme, url::DecodeUrlMode::kUtf8OrIsomorphic, &decoded);
  window.GetTasks()->PostTask(
      TaskType::kNetworking,
      base::BindOnce(&ExecuteJavaScriptUrl, std::u16string(decoded.view())));
}

}  // namespace

void Navigate(Window& window,
              const FrameLoadRequest& request,
              FrameLoadType type) {
  if (!CanProceedWithNavigation(window)) {
    return;
  }
  if (type == FrameLoadType::kStandard && !request.force_history_push &&
      ShouldReplaceCurrentEntry(window, request)) {
    type = FrameLoadType::kReplaceCurrentItem;
  }
  StartNavigation(window, request, type);
}

void StartNavigation(Window& window,
                     const FrameLoadRequest& request,
                     FrameLoadType type) {
  Document& document = window.GetDocument();
  const GURL& url = request.url;
  if (!url.is_valid()) {
    return;
  }
  // A top-level frame shows no data: URL that a page navigates it to, but one
  // that would be a download.
  if (url.SchemeIs(url::kDataScheme) &&
      request.reason != ClientNavigationReason::kReload &&
      IsDataUrlMimeTypeSupported(url)) {
    window.AddConsoleMessage(
        ConsoleLevel::kError,
        base::StrCat(
            {"Not allowed to navigate top frame to data URL: ", Elided(url)}));
    return;
  }
  // FrameNavigationDisabler and the page dismissal events: a navigation that
  // beforeunload, pagehide or unload starts does not start, nor one of a
  // frame while a new document commits in it, before any event.
  if (document.PageDismissalEventBeingDispatched()) {
    return;
  }
  if (const NavigationClient* const client = window.GetNavigationClient();
      client && client->IsCommitting()) {
    return;
  }
  // A navigation of the initial about:blank replaces it.
  if (type == FrameLoadType::kStandard && document.IsInitialEmptyDocument()) {
    type = FrameLoadType::kReplaceCurrentItem;
  }
  // ShouldPerformFragmentNavigation().
  if (type != FrameLoadType::kReload && url.has_ref() &&
      EqualIgnoringFragment(url, GURL(document.UrlForBinding()))) {
    CommitSameDocumentNavigation(window, request, type);
    return;
  }
  if (url.SchemeIs(url::kJavaScriptScheme)) {
    ProcessJavaScriptUrl(window, url);
    return;
  }
  // Mojo carries no URL longer than url::kMaxURLChars, so Chrome's browser
  // gets none and navigates to about:blank#blocked, which a Page does not:
  // the navigation starts nothing, a Known Gap. So a navigation's request
  // always fits in a frame.
  if (url.possibly_invalid_spec().size() > url::kMaxURLChars) {
    return;
  }
  // The navigate event, which may cancel the navigation, or intercept it,
  // which makes it one within the document, committed already.
  Navigation::NavigateParams params;
  params.url = url;
  params.load_type = type;
  params.source_element = request.source_element;
  if (window.Navigation()->DispatchNavigateEvent(params) !=
      Navigation::DispatchResult::kContinue) {
    return;
  }
  NavigationClient* const client = window.GetNavigationClient();
  if (!client || !window.HasContext()) {
    return;
  }
  // RenderFrameImpl::BeginNavigation(): beforeunload, which lets it go on,
  // then the document stops loading, as
  // CancelProvisionalLoaderForNewNavigation() aborts it, and the browser hears
  // of the navigation.
  if (!document.DispatchBeforeUnloadEvent()) {
    return;
  }
  document.Abort();
  NavigationRequest navigation;
  navigation.url = url;
  navigation.reload = type == FrameLoadType::kReload;
  navigation.replace = type == FrameLoadType::kReplaceCurrentItem;
  navigation.referrer_policy = document.GetReferrerPolicy();
  client->BeginNavigation(navigation);
}

bool CanProceedWithNavigation(Window& window) {
  return RateLimiterOf(window).CanProceed(window);
}

void AddStateObject(Window& window,
                    scoped_refptr<const SharedSerializedScriptValue> state,
                    const std::optional<std::u16string>& url,
                    FrameLoadType type,
                    bindings::ExceptionState& exception_state) {
  Document& document = window.GetDocument();
  const GURL document_url(document.UrlForBinding());
  // History::UrlForState(): the document's URL for none or "", and otherwise
  // the URL resolved against the document's base URL, as KURL(base, url) does.
  GURL full_url = document_url;
  std::string shown = document_url.possibly_invalid_spec();
  if (url && !url->empty()) {
    const ResolvedURL resolved(ResolvedURL(base::UTF8ToUTF16(
                                   document.BaseUrl().possibly_invalid_spec())),
                               *url);
    shown = base::UTF16ToUTF8(resolved.GetString());
    full_url = GURL(shown);
  }
  if (!CanChangeToUrlForHistoryApi(full_url, document.GetOrigin(),
                                   document_url)) {
    exception_state.ThrowDOMException(
        "SecurityError",
        base::StrCat({"A history state object with URL '", Elided(shown),
                      "' cannot be created in a document with origin '",
                      base::UTF16ToUTF8(window.Origin()), "' and URL '",
                      Elided(document_url), "'."}));
    return;
  }
  if (!CanProceedWithNavigation(window)) {
    return;
  }
  Navigation::NavigateParams params;
  params.url = full_url;
  params.history_api = true;
  params.load_type = type;
  params.state_object = state;
  if (window.Navigation()->DispatchNavigateEvent(params) !=
      Navigation::DispatchResult::kContinue) {
    return;
  }
  UpdateForSameDocumentNavigation(window, full_url, /*history_item=*/nullptr,
                                  SameDocumentNavigationType::kHistoryApi,
                                  std::move(state), type,
                                  /*fire_popstate=*/false);
}

void UpdateForSameDocumentNavigation(
    Window& window,
    const GURL& url,
    const HistoryItem* history_item,
    SameDocumentNavigationType navigation_type,
    scoped_refptr<const SharedSerializedScriptValue> state,
    FrameLoadType type,
    bool fire_popstate) {
  Document& document = window.GetDocument();
  // A new entry of the initial about:blank replaces it.
  if (type == FrameLoadType::kStandard && document.IsInitialEmptyDocument()) {
    type = FrameLoadType::kReplaceCurrentItem;
  }
  HistoryItem item;
  if (type == FrameLoadType::kBackForward && history_item) {
    item = *history_item;
    // The entry's state comes anew, as Blink reads the entry's state from the
    // browser's PageState for each traversal, so that history.state gives an
    // object of its own, not one that a script changed before it left.
    if (item.state) {
      item.state =
          base::MakeRefCounted<SharedSerializedScriptValue>(item.state->data);
    }
  } else {
    // As Blink's is_history_api_or_app_history_navigation: an intercepted
    // navigation is one of the history API, whose entry takes no Navigation
    // API state from the one before and has the classic state it gives, none
    // but that of pushState() or replaceState().
    const bool fragment =
        navigation_type == SameDocumentNavigationType::kFragment;
    item =
        HistoryItem::ForCommit(window.CurrentHistoryItem(), url, type,
                               fragment ? HistoryNavigationType::kFragment
                                        : HistoryNavigationType::kHistoryApi);
    if (!fragment) {
      item.state = std::move(state);
    }
    // A new entry protects its URL as the document's policy did when it
    // committed, which a meta element's later policy does not change.
    item.protect_url_in_navigation_api = document.ProtectsUrlInNavigationApi();
  }
  window.SetCurrentHistoryItem(item);
  document.SetUrl(base::UTF8ToUTF16(url.spec()));
  if (NavigationClient* const client = window.GetNavigationClient()) {
    client->DidCommitSameDocumentNavigation(item, type);
  }
  window.Navigation()->UpdateForNavigation(item, type);
  if (!fire_popstate || !window.HasContext()) {
    return;
  }
  // A traversal's popstate has the entry's state, the object that
  // history.state gives, and one to a fragment null.
  PopStateEvent* const popstate = PopStateEvent::CreateForHistory(
      history_item ? item.state : nullptr, window.History());
  window.DispatchEvent(popstate);
}

bool CommitSameDocumentHistoryNavigation(Window& window,
                                         const HistoryItem& item) {
  // An entry that the document is at already fires nothing.
  const HistoryItem* const current = window.CurrentHistoryItem();
  if (current && current->item_sequence_number == item.item_sequence_number) {
    return true;
  }
  Navigation::NavigateParams params;
  params.url = item.url;
  params.fragment = true;
  params.load_type = FrameLoadType::kBackForward;
  params.destination_item = &item;
  switch (window.Navigation()->DispatchNavigateEvent(params)) {
    case Navigation::DispatchResult::kAbort:
      return false;
    case Navigation::DispatchResult::kIntercept:
      return true;
    case Navigation::DispatchResult::kContinue:
      break;
  }
  Document& document = window.GetDocument();
  MaybeEnqueueHashChange(window, item.url, GURL(document.UrlForBinding()));
  UpdateForSameDocumentNavigation(
      window, item.url, &item, SameDocumentNavigationType::kFragment,
      /*state=*/nullptr, FrameLoadType::kBackForward,
      /*fire_popstate=*/true);
  if (window.HasContext()) {
    document.ProcessUrlFragment(item.url);
  }
  return true;
}

}  // namespace mitto
