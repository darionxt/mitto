// The objects that the bindings make of an interface without a C++ class whose
// nearest ancestor with a class is EventTarget or Event: an EventTarget or an
// Event whose wrapper is of that interface, so that the ancestor's members
// work on it, such as addEventListener() on an AbortSignal or `type` on a
// CustomEvent. Their own members stay Known Gaps (docs/runtime.md, "What a
// Known Gap does").

#ifndef MITTO_DOM_OBJECTS_BEHIND_H_
#define MITTO_DOM_OBJECTS_BEHIND_H_

#include "mitto/bindings/per_isolate_data.h"

namespace mitto {

// What PerIsolateData::SetObjectHooks() takes for the DOM's classes.
bindings::PerIsolateData::ObjectHooks ObjectsBehindHooks();

}  // namespace mitto

#endif  // MITTO_DOM_OBJECTS_BEHIND_H_
