#include "mitto/dom/frame_loader.h"

#include <stddef.h>

#include <string>
#include <string_view>
#include <tuple>

#include "base/check.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/dom/document.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/hash_change_event.h"
#include "mitto/dom/navigation.h"
#include "mitto/dom/navigation_client.h"
#include "mitto/dom/pop_state_event.h"
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

// NavigationRateLimiter: at most 200 navigations of a frame in 10 seconds,
// and a warning in the console for the first one past that in each period. A
// Page has one frame, so this is the process's.
class NavigationRateLimiter {
 public:
  bool CanProceed(Window& window) {
    const base::TimeTicks now = base::TimeTicks::Now();
    if (now - period_start_ > kPeriod) {
      period_start_ = now;
      count_ = 0;
    }
    if (count_ < kStateUpdateLimit) {
      ++count_;
      return true;
    }
    if (!warned_period_start_ || *warned_period_start_ != period_start_) {
      warned_period_start_ = period_start_;
      window.AddConsoleMessage(
          ConsoleLevel::kWarning,
          "Throttling navigation to prevent the browser from hanging. See "
          "https://crbug.com/1038223. Command line switch "
          "--disable-ipc-flooding-protection can be used to bypass the "
          "protection");
    }
    return false;
  }

 private:
  static constexpr base::TimeDelta kPeriod = base::Seconds(10);
  static constexpr int kStateUpdateLimit = 200;

  base::TimeTicks period_start_;
  int count_ = 0;
  std::optional<base::TimeTicks> warned_period_start_;
};

NavigationRateLimiter& RateLimiter() {
  static NavigationRateLimiter limiter;
  return limiter;
}

// Whether `a` and `b` are the same but for their fragments.
bool EqualIgnoringFragment(const GURL& a, const GURL& b) {
  return a.GetWithoutRef() == b.GetWithoutRef();
}

// KURL::ElidedString(): a URL longer than 1024 characters keeps its first
// 511 and its last 510, with "..." between them.
std::string Elided(const GURL& url) {
  const std::string& spec = url.possibly_invalid_spec();
  if (spec.size() <= 1024) {
    return spec;
  }
  return base::StrCat({std::string_view(spec).substr(0, 511), "...",
                       std::string_view(spec).substr(spec.size() - 510)});
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

// DocumentLoader::CommitSameDocumentNavigation(): the document takes `url` at
// once, the Session hears of it, popstate fires at the window with the
// entry's state, null for a fragment's, and hashchange from a task of the DOM
// manipulation task source if the fragment changed.
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
  const GURL old_url(document.UrlForBinding());
  const bool hash_change =
      EqualIgnoringFragment(url, old_url) &&
      (url.has_ref() != old_url.has_ref() || url.ref() != old_url.ref());
  if (hash_change && window.GetTasks()) {
    window.GetTasks()->PostTask(
        TaskType::kDOMManipulation,
        base::BindOnce(&FireHashChange, base::UTF8ToUTF16(old_url.spec()),
                       base::UTF8ToUTF16(url.spec())));
  }
  document.SetUrl(base::UTF8ToUTF16(url.spec()));
  if (NavigationClient* const client = window.GetNavigationClient()) {
    client->DidNavigateWithinDocument(url);
  }
  window.Navigation()->UpdateForNavigation(url, type, /*keep_state=*/true);
  if (!window.HasContext()) {
    return;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  bindings::PopStateEventInit init;
  init.state = v8::Null(isolate);
  PopStateEvent* const popstate = PopStateEvent::Create(u"popstate", init);
  popstate->SetTrusted(true);
  window.DispatchEvent(popstate);
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
  if (source.size() <= static_cast<size_t>(v8::String::kMaxLength) &&
      v8::String::NewFromTwoByte(
          isolate, reinterpret_cast<const uint16_t*>(source.data()),
          v8::NewStringType::kNormal, base::checked_cast<int>(source.size()))
          .ToLocal(&code) &&
      v8::Script::Compile(context, code).ToLocal(&script)) {
    std::ignore = script->Run(context);
  }
  if (try_catch.HasCaught() && try_catch.CanContinue()) {
    bindings::ReportException(isolate, try_catch.Exception(),
                              try_catch.Message());
  }
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
  if (!RateLimiter().CanProceed(window)) {
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
  // beforeunload, pagehide or unload starts does not start.
  if (document.PageDismissalEventBeingDispatched()) {
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
  client->BeginNavigation(navigation);
}

}  // namespace mitto
