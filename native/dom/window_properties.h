// The prototype of a window's prototype: the HTML Standard's named properties
// object, which finds elements of the window's document by name and ID. The
// bindings put its interceptors on WindowProperties' prototype object, as
// Blink's do, and ask the window of that object's realm.
//
// Upstream: third_party/blink/renderer/core/frame/window_properties.h
// Upstream: third_party/blink/renderer/core/frame/window_properties.cc

#ifndef MITTO_DOM_WINDOW_PROPERTIES_H_
#define MITTO_DOM_WINDOW_PROPERTIES_H_

#include <string>

#include "mitto/dom/event_target.h"
#include "v8/include/v8-forward.h"

namespace mitto {

class WindowProperties : public EventTarget {
 public:
  ~WindowProperties() override;

  // The named getter: the element of the document whose ID is `name`, if it
  // is the only element of that ID and no element has the name as a named
  // item, and otherwise the images, forms, embeds and objects of that name
  // and the elements of that ID, one as itself and more as a collection.
  // Empty for none. A child frame of the name comes first, as Blink's
  // FrameTree::ScopedChild() finds it.
  v8::Local<v8::Value> AnonymousNamedGetter(const std::u16string& name);

 protected:
  WindowProperties();
};

}  // namespace mitto

#endif  // MITTO_DOM_WINDOW_PROPERTIES_H_
