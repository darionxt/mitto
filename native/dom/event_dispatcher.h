// Dispatches an event at a node through the tree, as the DOM Standard's
// dispatch and Blink's EventDispatcher do: the capturing listeners of the
// window and of each node from the root down, the target's, then the bubbling
// listeners back up to the window, and for a click, the activation behaviour
// of its activation target and the default event handlers.
//
// Upstream: third_party/blink/renderer/core/dom/events/event_dispatcher.h
// Upstream: third_party/blink/renderer/core/dom/events/event_dispatcher.cc
// Upstream: third_party/blink/renderer/core/events/simulated_event_util.cc

#ifndef MITTO_DOM_EVENT_DISPATCHER_H_
#define MITTO_DOM_EVENT_DISPATCHER_H_

#include "mitto/dom/event.h"
#include "mitto/dom/event_target.h"
#include "mitto/dom/node.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class Event;
class Node;

// What a node's legacy-pre-activation behaviour hands to the rest of its
// activation behaviour, as Blink's EventDispatchHandlingState does, such as
// the state that a checkbox had before a click toggled it.
class EventDispatchHandlingState
    : public cppgc::GarbageCollected<EventDispatchHandlingState> {
 public:
  virtual ~EventDispatchHandlingState() = default;
  virtual void Trace(cppgc::Visitor* visitor) const {}
};

// Lives on the stack for one dispatch.
class EventDispatcher final {
 public:
  // Dispatches `event` at `node`.
  static DispatchEventResult DispatchEvent(Node& node, Event& event);
  // Dispatches the click that the HTML Standard's click() fires at `node`:
  // a PointerEvent that bubbles and that its listeners can cancel, which no
  // person started, and nothing at a node already in the middle of one.
  static void DispatchSimulatedClick(Node& node);

  EventDispatcher(const EventDispatcher&) = delete;
  EventDispatcher& operator=(const EventDispatcher&) = delete;
  ~EventDispatcher();

 private:
  enum class Continuation { kContinue, kDone };

  EventDispatcher(Node& node, Event& event);

  DispatchEventResult Dispatch();
  Continuation DispatchEventLegacyPreActivationBehavior(
      Node* activation_target,
      EventDispatchHandlingState*& pre_dispatch_state);
  Continuation DispatchEventAtCapturing();
  void DispatchEventAtBubbling();
  void DispatchEventPostProcess(Node* activation_target,
                                EventDispatchHandlingState* pre_dispatch_state);

  // Held while scripts run, which the collector runs beside.
  const cppgc::Persistent<Node> node_;
  const cppgc::Persistent<Event> event_;
};

}  // namespace mitto

#endif  // MITTO_DOM_EVENT_DISPATCHER_H_
