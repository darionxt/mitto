// The frames of other sites, which Chrome's site isolation keeps in processes
// of their own. A document has such a frame as a RemoteFrame, whose
// RemoteDOMWindow stays the frame's while its documents come and go, until one
// of the document's own site takes the frame into this process, where it has a
// LocalDOMWindow for each of them. A Page's documents are all in one process,
// so a window here is its document's alone, and what a script of another site
// would have of its frame instead is this.
//
// Upstream: third_party/blink/renderer/core/frame/remote_dom_window.h
// Upstream: content/browser/site_info.cc

#ifndef MITTO_DOM_REMOTE_WINDOW_H_
#define MITTO_DOM_REMOTE_WINDOW_H_

#include "url/origin.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class Window;

// Whether Chrome has documents of the two origins in one process: those of one
// site, where an opaque origin counts as the one it was made for. Known Gap: a
// document that asks for a process of its origin's own, with an
// Origin-Agent-Cluster header, counts with its site's.
bool IsInSameProcess(const url::Origin& a, const url::Origin& b);

// The window of a frame in the process of `viewer`'s documents, which is not
// its own document's: Blink's RemoteDOMWindow.
class RemoteWindow final : public cppgc::GarbageCollected<RemoteWindow> {
 public:
  // What `window`'s frame is to a document of `viewer`, or null if they are in
  // one process, or the window is not its frame's now.
  static RemoteWindow* From(const Window& window, const url::Origin& viewer);

  RemoteWindow(url::Origin viewer, Window* window);

  RemoteWindow(const RemoteWindow&) = delete;
  RemoteWindow& operator=(const RemoteWindow&) = delete;

  ~RemoteWindow();

  void Trace(cppgc::Visitor* visitor) const;

  const url::Origin& Viewer() const { return viewer_; }
  // The frame's window, its last if it was removed, whose WindowProxy is the
  // frame's. Null once the frame has shown a document of the viewer's process,
  // and so has another window there.
  Window* CurrentWindow() const { return window_.Get(); }
  void SetCurrentWindow(Window* window) { window_ = window; }

 private:
  const url::Origin viewer_;
  cppgc::Member<Window> window_;
};

}  // namespace mitto

#endif  // MITTO_DOM_REMOTE_WINDOW_H_
