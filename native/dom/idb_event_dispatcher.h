// How IndexedDB fires its events, as Blink's IDBEventDispatcher does: an
// event at a request goes on to its transaction and its connection, capturing
// from the connection down and bubbling back up, and one at a transaction to
// its connection. And how a task of IndexedDB's runs in a window's context.
//
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_event_dispatcher.h
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_event_dispatcher.cc

#ifndef MITTO_DOM_IDB_EVENT_DISPATCHER_H_
#define MITTO_DOM_IDB_EVENT_DISPATCHER_H_

#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/functional/function_ref.h"
#include "mitto/dom/event_target.h"

namespace mitto {

class Event;
class Window;

// Dispatches `event` along `targets`, the target first and each parent after
// it, which outlive the dispatch.
DispatchEventResult DispatchIDBEvent(Event& event,
                                     const std::vector<EventTarget*>& targets);

// A trusted event of `type`, as Blink's Event::Create(), CreateBubble() and
// CreateCancelableBubble() make the events that IndexedDB fires.
Event* MakeIDBEvent(const std::u16string& type, bool bubbles, bool cancelable);

// Runs `run` now in `window`'s context, and returns whether the window still
// had its context. Unlike RunInWindowContext(), nothing holds microtasks back
// around it: as in Blink, where IndexedDB fires its events from tasks of its
// own, the microtasks that each listener queues run once it returns, while
// the request's transaction is still active, and the rest at the task's end.
bool RunInIDBContext(Window* window, base::FunctionRef<void()> run);

// Runs `task` in `window`'s context from a later task of the database access
// queue, as Blink posts to the context's kDatabaseAccess task runner.
void PostIDBTask(Window* window, base::OnceClosure task);

}  // namespace mitto

#endif  // MITTO_DOM_IDB_EVENT_DISPATCHER_H_
