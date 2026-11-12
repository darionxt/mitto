// The event handler attributes of a page's EventTargets, such as
// window.onload, which the bindings reach through PerIsolateData's hooks: each
// is the listener of an EventTarget, which its dispatch runs. An
// HTMLBodyElement's handlers of the window's events are its window's.

#ifndef MITTO_DOM_EVENT_HANDLERS_H_
#define MITTO_DOM_EVENT_HANDLERS_H_

#include "mitto/bindings/per_isolate_data.h"

namespace mitto {

// Fills the event handler hooks of `hooks`.
void AddEventHandlerHooks(bindings::PerIsolateData::ObjectHooks& hooks);

}  // namespace mitto

#endif  // MITTO_DOM_EVENT_HANDLERS_H_
