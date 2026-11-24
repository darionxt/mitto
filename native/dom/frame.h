// A frame of a Page as the DOM sees it, Blink's Frame with its FrameTree: a
// browsing context with a window, a place in the Page's frame tree, the
// element that holds it in its parent's document, and a name. The Page
// program's frames are its frames (page_frame.h).
//
// Upstream: third_party/blink/renderer/core/frame/frame.h
// Upstream: third_party/blink/renderer/core/page/frame_tree.cc

#ifndef MITTO_DOM_FRAME_H_
#define MITTO_DOM_FRAME_H_

#include <stddef.h>

#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/memory/raw_ref.h"
#include "mitto/dom/sandbox_flags.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "v8/include/cppgc/persistent.h"

namespace mitto {

class HTMLFrameOwnerElement;
class NavigationRateLimiter;
class RemoteWindow;
struct RendererProcess;
class Window;

class Frame {
 public:
  Frame(const Frame&) = delete;
  Frame& operator=(const Frame&) = delete;

  // The frame's current window.
  virtual Window* DomWindow() const = 0;
  // The frame whose document holds this one's element, or null for the
  // top-level frame.
  virtual Frame* Parent() const = 0;
  // The frame's children, in the order they were made, as Blink's FrameTree
  // keeps them, which window[i] reads.
  virtual size_t ChildCount() const = 0;
  virtual Frame* ChildAt(size_t index) const = 0;
  // The element that holds the frame, or null for the top-level frame.
  virtual HTMLFrameOwnerElement* Owner() const = 0;
  // The browsing context's name, which window.name reads and writes, and
  // which it keeps across its documents.
  virtual const std::u16string& Name() const = 0;
  virtual void SetName(std::u16string name) = 0;
  // Whether the frame still loads its document, which its parent's load
  // event waits for, as Blink's Frame::IsLoading() says.
  virtual bool IsLoading() const = 0;

  // LocalFrameClient::CreateFrame(): a new child of this frame, held by
  // `owner` and named `name`, whose first window has an initial about:blank of
  // this frame's document's origin. Null if this frame can have no more.
  virtual Frame* CreateChildFrame(HTMLFrameOwnerElement& owner,
                                  std::u16string name) = 0;
  // FrameLoader::StartNavigation() for a navigation that the frame's element
  // starts to `url`, as its src gives it, from its parent's window.
  virtual void NavigateFromOwner(const GURL& url) = 0;
  // HTMLFrameOwnerElement::DisconnectContentFrame(): the frame's documents
  // unload, its window's context goes, and it leaves the frame tree, with
  // its own children. The frame lives on until the task ends, for a script
  // of it that still runs, and so do its window's timers, callbacks and
  // fetches, which stop at once.
  virtual void Detach() = 0;
  // What counts the navigations that the frame's documents start, which it
  // keeps across them, as Blink's Frame::navigation_rate_limiter().
  virtual NavigationRateLimiter& GetNavigationRateLimiter() = 0;

  // The frame's window in the process of `viewer`'s documents, another than
  // its own document's, the same for as long as a message holds it.
  RemoteWindow* RemoteWindowFor(const RendererProcess& viewer);
  // The top-level frame of this one's tree.
  Frame& Top();
  // FrameTree::ScopedChild(): the first child whose name is `name`, or null.
  // No child has an empty name.
  Frame* ScopedChild(std::u16string_view name) const;
  // Whether this frame is `other` or one of its descendants.
  bool IsDescendantOf(const Frame& other) const;
  // What the frame withholds from a document that it navigates to now: its
  // element's sandbox, and what its parent's document has withheld, as Chrome's
  // FrameTreeNode::SetPendingFramePolicy() puts them together.
  SandboxFlags PendingSandboxFlags() const;
  // Whether the console has not been told `text` through this frame, the
  // top-level one, before.
  bool AddOneTimeMessage(const std::string& text) {
    return one_time_messages_.insert(text).second;
  }

  // Blink's LocalFrame::IsNavigationAllowed(): no navigation of the frame
  // starts while a FrameNavigationDisabler holds it.
  bool IsNavigationAllowed() const { return navigation_disable_count_ == 0; }

 protected:
  Frame();
  virtual ~Frame();

  // The frame shows the document of `window`, whose window takes the place of
  // the frame's remote one in its process.
  void DidShowDocument(Window& window);

 private:
  friend class FrameNavigationDisabler;

  int navigation_disable_count_ = 0;
  std::set<std::string> one_time_messages_;
  // One for each process, for the frame's document now.
  std::vector<cppgc::WeakPersistent<RemoteWindow>> remote_windows_;
};

// Blink's FrameNavigationDisabler: while a frame's document commits, a script
// of its old document or of its frames, which unload, starts no navigation of
// it.
class FrameNavigationDisabler {
 public:
  explicit FrameNavigationDisabler(Frame& frame) : frame_(frame) {
    ++frame_->navigation_disable_count_;
  }
  FrameNavigationDisabler(const FrameNavigationDisabler&) = delete;
  FrameNavigationDisabler& operator=(const FrameNavigationDisabler&) = delete;
  ~FrameNavigationDisabler() { --frame_->navigation_disable_count_; }

 private:
  const raw_ref<Frame> frame_;
};

}  // namespace mitto

#endif  // MITTO_DOM_FRAME_H_
