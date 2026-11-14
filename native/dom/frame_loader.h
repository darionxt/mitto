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

#ifndef MITTO_DOM_FRAME_LOADER_H_
#define MITTO_DOM_FRAME_LOADER_H_

#include "url/gurl.h"

namespace mitto {

class Window;

// Blink's WebFrameLoadType, which the session history and the Navigation API
// see: a new entry, the current one replaced, or the document reloaded.
enum class FrameLoadType {
  kStandard,
  kReplaceCurrentItem,
  kReload,
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

}  // namespace mitto

#endif  // MITTO_DOM_FRAME_LOADER_H_
