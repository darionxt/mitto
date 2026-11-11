// The DOM's EventTarget: an object that keeps listeners and dispatches events
// to them.
//
// It follows Blink's EventTarget, for dispatch on a single target: the event's
// path is the target alone, where every listener of the event's type runs, in
// the order they were added, capturing or not, as Blink runs them on a target
// that is not a node.
//
// Upstream: third_party/blink/renderer/core/dom/events/event_target.h
// Upstream: third_party/blink/renderer/core/dom/events/event_target.cc

#ifndef MITTO_DOM_EVENT_TARGET_H_
#define MITTO_DOM_EVENT_TARGET_H_

#include <optional>
#include <string>
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

class Event;

// One listener that a page added: the DOM standard's event listener.
class EventListenerEntry final
    : public cppgc::GarbageCollected<EventListenerEntry> {
 public:
  EventListenerEntry(v8::Isolate* isolate,
                     const std::u16string& type,
                     v8::Local<v8::Object> callback,
                     bool capture,
                     bool passive,
                     bool once);
  ~EventListenerEntry();

  void Trace(cppgc::Visitor* visitor) const;

  const std::u16string& type() const { return type_; }
  v8::Local<v8::Object> callback(v8::Isolate* isolate) const {
    return callback_.Get(isolate);
  }
  bool capture() const { return capture_; }
  bool passive() const { return passive_; }
  bool once() const { return once_; }
  bool removed() const { return removed_; }
  void set_removed() { removed_ = true; }

 private:
  const std::u16string type_;
  v8::TracedReference<v8::Object> callback_;
  const bool capture_;
  const bool passive_;
  const bool once_;
  bool removed_ = false;
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

  // Dispatches `event` to this target's listeners, and returns whether no
  // listener canceled it.
  bool DispatchEvent(Event* event);

 private:
  // Runs the listeners of the event's type, and returns whether the event is
  // canceled, as Blink's EventTarget::FireEventListeners() does.
  bool FireEventListeners(Event* event);
  void RemoveEntry(EventListenerEntry* entry);

  std::vector<cppgc::Member<EventListenerEntry>> listeners_;
  // Whether Blink would have made this target's EventTargetData, which it does
  // when a page first adds a listener.
  bool has_event_target_data_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_EVENT_TARGET_H_
