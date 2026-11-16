// The popover state of an HTML element whose popover attribute makes it a
// popover: its type, whether it shows, the element that showed it, and its
// queued toggle event.
//
// Upstream: third_party/blink/renderer/core/html/popover_data.h

#ifndef MITTO_DOM_POPOVER_DATA_H_
#define MITTO_DOM_POPOVER_DATA_H_

#include <stdint.h>

#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

class Element;

// What the popover attribute's value makes an element, as Blink's
// GetPopoverTypeFromAttributeValue() reads it.
enum class PopoverValueType { kNone, kAuto, kHint, kManual };

enum class PopoverVisibilityState { kHidden, kShowing };

class PopoverData final : public cppgc::GarbageCollected<PopoverData> {
 public:
  explicit PopoverData(PopoverValueType type);
  ~PopoverData();
  void Trace(cppgc::Visitor* visitor) const;

  PopoverValueType type() const { return type_; }
  void set_type(PopoverValueType type) { type_ = type; }
  PopoverVisibilityState visibility_state() const { return visibility_state_; }
  void set_visibility_state(PopoverVisibilityState state) {
    visibility_state_ = state;
  }
  Element* invoker() const { return invoker_.Get(); }
  void set_invoker(Element* invoker) { invoker_ = invoker; }
  bool hiding_this_popover() const { return hiding_this_popover_; }
  void set_hiding_this_popover(bool hiding) { hiding_this_popover_ = hiding; }

  // The task that dispatches the queued toggle event, as Blink's
  // PostCancellableTask() handle: 0 for none, and another number once a
  // newer event cancels it.
  uint64_t pending_toggle_event_task() const {
    return pending_toggle_event_task_;
  }
  void set_pending_toggle_event_task(uint64_t task) {
    pending_toggle_event_task_ = task;
  }
  bool has_pending_toggle_event_task() const {
    return pending_toggle_event_task_ != 0;
  }
  void cancel_pending_toggle_event_task() { pending_toggle_event_task_ = 0; }
  // Whether the popover was closed when its queued toggle event was first
  // queued, which the old state of a newer one keeps.
  bool pending_toggle_event_started_closed() const {
    return pending_toggle_event_started_closed_;
  }
  void set_pending_toggle_event_started_closed(bool closed) {
    pending_toggle_event_started_closed_ = closed;
  }

 private:
  PopoverValueType type_;
  PopoverVisibilityState visibility_state_ = PopoverVisibilityState::kHidden;
  cppgc::Member<Element> invoker_;
  bool hiding_this_popover_ = false;
  uint64_t pending_toggle_event_task_ = 0;
  bool pending_toggle_event_started_closed_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_POPOVER_DATA_H_
