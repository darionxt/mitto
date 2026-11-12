// CSS Viewport's Viewport, the window's viewport: its segments are the one of
// a window on one display, which is the whole viewport, a new array of a new
// DOMRect on each read, as Blink makes them.
//
// Upstream: third_party/blink/renderer/core/frame/dom_viewport.cc

#ifndef MITTO_DOM_VIEWPORT_H_
#define MITTO_DOM_VIEWPORT_H_

#include "mitto/bindings/script_wrappable.h"
#include "mitto/page_messages.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-value.h"

namespace mitto {

class Viewport final : public bindings::ScriptWrappable {
 public:
  static Viewport* Create(const PageStart& start);
  // One of no size, for a Known Gap that gives one.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  explicit Viewport(const PageStart& start);
  ~Viewport() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  v8::Local<v8::Value> Segments() const;

 private:
  const int width_;
  const int height_;
};

}  // namespace mitto

#endif  // MITTO_DOM_VIEWPORT_H_
