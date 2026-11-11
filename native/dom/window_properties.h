// The prototype of a window's prototype: the HTML Standard's named properties
// object, which finds elements of the document by name and id. Its named
// getter is a Known Gap: the Runtime installs no interceptor for it yet.
//
// Upstream: third_party/blink/renderer/core/frame/window_properties.h

#ifndef MITTO_DOM_WINDOW_PROPERTIES_H_
#define MITTO_DOM_WINDOW_PROPERTIES_H_

#include "mitto/dom/event_target.h"

namespace mitto {

class WindowProperties : public EventTarget {
 public:
  ~WindowProperties() override;

 protected:
  WindowProperties();
};

}  // namespace mitto

#endif  // MITTO_DOM_WINDOW_PROPERTIES_H_
