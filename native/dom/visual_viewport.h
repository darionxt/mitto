// CSSOM View's VisualViewport, the window's visualViewport: the viewport of a
// page that is neither zoomed nor scrolled, as large as the Machine's
// viewport, since a Page lays nothing out that would overflow it (roadmap,
// M9). No resize or scroll event comes.
//
// Upstream: third_party/blink/renderer/core/frame/dom_visual_viewport.cc

#ifndef MITTO_DOM_VISUAL_VIEWPORT_H_
#define MITTO_DOM_VISUAL_VIEWPORT_H_

#include "mitto/dom/event_target.h"
#include "mitto/page_messages.h"

namespace mitto {

class VisualViewport final : public EventTarget {
 public:
  static VisualViewport* Create(const PageStart& start);
  // One of a viewport of no size, for a Known Gap that gives one.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  explicit VisualViewport(const PageStart& start);
  ~VisualViewport() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  double OffsetLeft() const { return 0; }
  double OffsetTop() const { return 0; }
  double PageLeft() const { return 0; }
  double PageTop() const { return 0; }
  double Width() const { return width_; }
  double Height() const { return height_; }
  double Scale() const { return 1; }

 private:
  const double width_;
  const double height_;
};

}  // namespace mitto

#endif  // MITTO_DOM_VISUAL_VIEWPORT_H_
