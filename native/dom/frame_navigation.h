// Which frame a link or window.open() names as its target, and whether a
// frame's document may navigate it, as Blink's FrameTree and LocalFrame have
// it.
//
// Upstream: third_party/blink/renderer/core/page/frame_tree.cc
// Upstream: third_party/blink/renderer/core/page/create_window.cc
// Upstream: third_party/blink/renderer/core/frame/local_frame.cc

#ifndef MITTO_DOM_FRAME_NAVIGATION_H_
#define MITTO_DOM_FRAME_NAVIGATION_H_

#include <string_view>

#include "url/gurl.h"

namespace mitto {

class Frame;
class Window;

// LocalFrame::CanNavigate(): whether the document of `source` may navigate
// `target` to `url`, and if not, its console says why. A Page has no user
// activation, so nothing that takes one may.
bool CanNavigate(Frame& source, Frame& target, const GURL& url);

// SecurityOrigin::CanDisplay(): whether the document of `window` may show
// `url`, and if not, its console says why: no blob: URL of another origin.
bool CanDisplay(Window& window, const GURL& url);

// FrameTree::FindOrCreateFrameForNavigation(): the frame that `name` names for
// a navigation of `source` to `url`, or null for one that it may not navigate,
// and for a new window, which a sandbox withholds, with its error in the
// console, or Chrome's pop-up blocker refuses.
Frame* FindFrameForNavigation(Frame& source,
                              std::u16string_view name,
                              const GURL& url);

}  // namespace mitto

#endif  // MITTO_DOM_FRAME_NAVIGATION_H_
