// How a window's frame navigates when its document starts it, through
// location, a link, a refresh, window.open() or the Navigation API, as Blink's
// LocalFrame::Navigate() and FrameLoader::StartNavigation() have it: a
// navigation within the document commits at once, a javascript: URL runs from
// a task, and a navigation to another document goes to the Session once the
// document has fired beforeunload and stopped loading. Its response, unless it
// has no document, takes the place of the window's document later, in the Page
// program (page_frame.h).
//
// Upstream: third_party/blink/renderer/core/frame/local_frame.cc
// Upstream: third_party/blink/renderer/core/frame/navigation_rate_limiter.cc
// Upstream: third_party/blink/renderer/core/loader/frame_loader.cc
// Upstream: third_party/blink/renderer/core/loader/document_loader.cc
// Upstream: third_party/blink/renderer/core/frame/history.cc

#ifndef MITTO_DOM_FRAME_LOADER_H_
#define MITTO_DOM_FRAME_LOADER_H_

#include <memory>
#include <optional>
#include <string>

#include "base/memory/raw_ptr_exclusion.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/dom/serialized_script_value.h"
#include "url/gurl.h"

namespace mitto {

class Element;
struct HistoryItem;
class Window;

// Blink's WebFrameLoadType, which the session history and the Navigation API
// see: a new entry, the current one replaced, the document reloaded, or a
// traversal to an entry that the session history has.
enum class FrameLoadType {
  kStandard,
  kReplaceCurrentItem,
  kReload,
  kBackForward,
};

// Blink's ClientNavigationReason: what started a navigation.
enum class ClientNavigationReason {
  // window.open() of a frame that is there already.
  kNone,
  // location and the Navigation API.
  kFrameNavigation,
  // A link.
  kAnchorClick,
  // location.reload().
  kReload,
  kMetaTagRefresh,
  kHttpHeaderRefresh,
};

// What Blink's FrameLoadRequest holds of a navigation that a document starts,
// whose Initiator is the document.
struct FrameLoadRequest {
  GURL url;
  ClientNavigationReason reason = ClientNavigationReason::kFrameNavigation;
  // The link that started it, which its navigate event names.
  // On the stack only, as the garbage collector scans it.
  RAW_PTR_EXCLUSION Element* source_element = nullptr;
  // navigate() with history "push", which keeps a new entry where a
  // navigation would otherwise replace the current one.
  bool force_history_push = false;
};

// LocalFrame::Navigate(): drops a navigation past the rate limit, makes a
// standard one replace the current entry where Blink does, and starts it.
void Navigate(Window& window,
              const FrameLoadRequest& request,
              FrameLoadType type);

// FrameLoader::StartNavigation(): commits a navigation within the document,
// runs a javascript: URL from a task, or has the Session start a navigation
// to another document, as the comment at the top says.
void StartNavigation(Window& window,
                     const FrameLoadRequest& request,
                     FrameLoadType type);

// NavigationRateLimiter::CanProceed() for the window's frame: whether one more
// navigation, pushState(), replaceState() or history.go() may go, at most 200
// in 10 seconds, with a warning in the console for the first past that.
bool CanProceedWithNavigation(Window& window);

// History::StateObjectAdded(): the rest of pushState() and replaceState(),
// once their data has been serialized as `state` and the window is known to be
// fully active: the URL, `url` resolved against the document's base URL, or
// the document's own for none, which the document must be able to take, or
// this throws Blink's SecurityError; the rate limit; the navigate event; then
// the URL and history update steps, with no popstate.
void AddStateObject(Window& window,
                    std::shared_ptr<const SerializedScriptValue> state,
                    const std::optional<std::u16string>& url,
                    FrameLoadType type,
                    bindings::ExceptionState& exception_state);

// Blink's mojom::SameDocumentNavigationType, as far as a Page has them.
enum class SameDocumentNavigationType : uint8_t {
  kFragment,
  kHistoryApi,
  kNavigationApiIntercept,
};

// DocumentLoader::UpdateForSameDocumentNavigation(): the document takes `url`
// and an entry, `history_item` for a traversal, and otherwise a new one or one
// in place of the current one, as HistoryItem::ForCommit() makes it, with
// `state` for pushState() and replaceState(); the session history and the
// Session hear of it, the Navigation API's entries change, and popstate fires
// with the entry's state if `fire_popstate`.
void UpdateForSameDocumentNavigation(
    Window& window,
    const GURL& url,
    const HistoryItem* history_item,
    SameDocumentNavigationType navigation_type,
    std::shared_ptr<const SerializedScriptValue> state,
    FrameLoadType type,
    bool fire_popstate);

// What a same-document traversal does with the window's document, as Blink's
// DocumentLoader::CommitSameDocumentNavigation() has it for a kBackForward
// load: nothing for the entry it is at already; otherwise the navigate event,
// which may cancel it, then hashchange queued if the fragment changes, the
// entry, its state and URL, currententrychange and the Navigation API's
// promises, popstate, and the fragment's target. Returns false if the navigate
// event canceled it, and the session history stays where it was.
bool CommitSameDocumentHistoryNavigation(Window& window,
                                         const HistoryItem& item);

}  // namespace mitto

#endif  // MITTO_DOM_FRAME_LOADER_H_
