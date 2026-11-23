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

#include <string>
#include <string_view>

#include "url/gurl.h"

namespace mitto {

class HTMLFrameOwnerElement;
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

  // The top-level frame of this one's tree.
  Frame& Top();
  // FrameTree::ScopedChild(): the first child whose name is `name`, or null.
  // No child has an empty name.
  Frame* ScopedChild(std::u16string_view name) const;
  // Whether this frame is `other` or one of its descendants.
  bool IsDescendantOf(const Frame& other) const;

 protected:
  Frame() = default;
  virtual ~Frame() = default;
};

}  // namespace mitto

#endif  // MITTO_DOM_FRAME_H_
