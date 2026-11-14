// The DOM's EventTarget: an object that keeps listeners and dispatches events
// to them.
//
// It follows Blink's EventTarget: a target that is not a node dispatches an
// event to its own listeners alone, every listener of the event's type in the
// order they were added, capturing or not, and a node through the tree, which
// event_dispatcher.h does, running each target's listeners here.
//
// Upstream: third_party/blink/renderer/core/dom/events/event_target.h
// Upstream: third_party/blink/renderer/core/dom/events/event_target.cc
// Upstream: third_party/blink/renderer/core/dom/abort_signal_registry.cc

#ifndef MITTO_DOM_EVENT_TARGET_H_
#define MITTO_DOM_EVENT_TARGET_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-traced-handle.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class AbortSignal;
class Event;
class Node;
class Window;

// What a dispatch did with an event, as Blink's DispatchEventResult says.
enum class DispatchEventResult {
  kNotCanceled,
  // A listener canceled it.
  kCanceledByEventHandler,
  // A default event handler handled it, as DOMActivate that a listener
  // canceled marks its click.
  kCanceledByDefaultEventHandler,
};

// One listener that a page added: the DOM standard's event listener.
class EventListenerEntry final
    : public cppgc::GarbageCollected<EventListenerEntry> {
 public:
  EventListenerEntry(v8::Isolate* isolate,
                     const std::u16string& type,
                     v8::Local<v8::Object> callback,
                     bool capture,
                     bool passive,
                     bool once,
                     bool attribute_handler = false,
                     bool passive_specified = false,
                     bool passive_forced_for_document_target = false);
  ~EventListenerEntry();

  void Trace(cppgc::Visitor* visitor) const;

  const std::u16string& type() const { return type_; }
  v8::Local<v8::Object> callback(v8::Isolate* isolate) const {
    return callback_.Get(isolate);
  }
  bool capture() const { return capture_; }
  bool passive() const { return passive_; }
  bool once() const { return once_; }
  // Whether the page said `passive`, and whether Blink made the listener
  // passive for a document's scrolling, as it does a touch or wheel listener
  // of a window, a document, its root element or its body.
  bool passive_specified() const { return passive_specified_; }
  bool passive_forced_for_document_target() const {
    return passive_forced_for_document_target_;
  }
  bool removed() const { return removed_; }
  void set_removed() { removed_ = true; }
  // Whether it is the listener of an event handler attribute, such as
  // `onload`, whose callback is the handler that the attribute was set to.
  bool attribute_handler() const { return attribute_handler_; }
  // Sets an event handler attribute's handler again, keeping its place.
  void set_callback(v8::Isolate* isolate, v8::Local<v8::Object> callback) {
    callback_.Reset(isolate, callback);
  }

  // The next listener of the target, in the order they were added.
  EventListenerEntry* next() const { return next_.Get(); }
  void set_next(EventListenerEntry* next) { next_ = next; }

  // The signal that removes it, which it keeps while the target keeps it, as
  // Blink's AbortSignalRegistry keeps the signal's algorithm while its
  // listener lives, so that a signal that only listeners reach stays.
  void set_signal(AbortSignal* signal);

 private:
  const std::u16string type_;
  v8::TracedReference<v8::Object> callback_;
  const bool capture_;
  const bool passive_;
  const bool once_;
  const bool attribute_handler_;
  const bool passive_specified_;
  const bool passive_forced_for_document_target_;
  bool removed_ = false;
  cppgc::Member<EventListenerEntry> next_;
  cppgc::Member<AbortSignal> signal_;
};

class EventTarget : public bindings::ScriptWrappable {
 public:
  static EventTarget* Create();

  EventTarget();
  ~EventTarget() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  void AddEventListener(
      const std::u16string& type,
      v8::Local<v8::Object> listener,
      const std::optional<bindings::AddEventListenerOptionsOrBoolean>& options);
  void RemoveEventListener(
      const std::u16string& type,
      v8::Local<v8::Object> listener,
      const std::optional<bindings::EventListenerOptionsOrBoolean>& options);
  bool DispatchEventForBindings(Event* event,
                                bindings::ExceptionState& exception_state);

  // This target as a node or a window, or null.
  virtual Node* ToNode();
  virtual Window* ToWindow();

  // Dispatches `event`, through the tree for a node, and returns whether
  // nothing canceled it.
  bool DispatchEvent(Event* event);
  // The same, and what canceled it.
  DispatchEventResult DispatchEventWithResult(Event* event);
  // At this target alone, with `target` as the event's target, as Blink's
  // LocalDOMWindow::DispatchEvent() fires an event at a window with the HTML
  // Standard's legacy target override flag, such as load, whose target is the
  // window's document. As there, the event keeps this target as its current
  // target, and its target phase, once it is done.
  bool DispatchEventWithTarget(Event* event, EventTarget* target);

  // Runs this target's listeners for the event, at the event's phase, as
  // Blink's EventTarget::FireEventListeners() does. Returns whether any ran.
  bool FireEventListeners(Event* event);
  // What a dispatch of `event` did, once it is done.
  static DispatchEventResult GetDispatchEventResult(const Event& event);

  // The handler of the event handler attribute for `type`, such as `onload`
  // for load, or null.
  v8::Local<v8::Value> AttributeEventHandler(v8::Isolate* isolate,
                                             std::u16string_view type) const;
  // Sets it, as the HTML Standard's event handler attributes do: its listener
  // joins the others the first time, keeps its place when it is set again, and
  // goes when it is set to null, for which `handler` is not an object.
  void SetAttributeEventHandler(v8::Isolate* isolate,
                                const std::u16string& type,
                                v8::Local<v8::Value> handler);

  // Whether a listener of `type` is there, an event handler's among them.
  bool HasEventListeners(std::u16string_view type) const;
  // Removes the listener of `type` with `callback` and `capture`, if there is
  // one, as removeEventListener() does.
  void RemoveListener(const std::u16string& type,
                      v8::Local<v8::Object> callback,
                      bool capture);
  // The same, for the function that an abort algorithm marked `listener_id`.
  void RemoveMarkedListener(const std::u16string& type,
                            double listener_id,
                            bool capture);
  // Removes every listener, its event handlers' among them, as Blink's
  // EventTarget::RemoveAllEventListeners() does for document.open().
  void RemoveAllEventListeners();

 protected:
  // Called once a listener of `type` has joined or left, as Blink's
  // AddedEventListener() and RemovedEventListener() are.
  virtual void ListenersChanged(std::u16string_view type) {}
  // Dispatches `event`: at this target alone, as Blink's
  // EventTarget::DispatchEventInternal() does. A node goes through the tree.
  virtual DispatchEventResult DispatchEventInternal(Event* event);

 private:
  // Blink's IsTopLevelNode(): a window, a document, its root element or its
  // body, whose touch and wheel listeners are passive by default.
  bool IsTopLevelNode();
  void RemoveEntry(EventListenerEntry* entry);

  // The listeners, in the order they were added, as a list of objects on the
  // C++ heap. V8's collector traces objects on threads of its own while
  // scripts run, which is safe for a Member in an object on the heap, but not
  // for one in a std::vector's buffer, which the main thread can reallocate
  // meanwhile. Blink has HeapVector, whose buffer is on the heap; cppgc alone
  // has no such vector.
  cppgc::Member<EventListenerEntry> first_listener_;
  cppgc::Member<EventListenerEntry> last_listener_;
  // Whether Blink would have made this target's EventTargetData, which it does
  // when a page first adds a listener.
  bool has_event_target_data_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_EVENT_TARGET_H_
