// Prioritized Task Scheduling's TaskSignal, as Blink's DOMTaskSignal: an
// AbortSignal with a priority, which a TaskController gives. Changing the
// priority, TaskSignal.any() and the tasks it would prioritize are Known
// Gaps, so a signal keeps the priority that it was made with.
//
// Upstream: third_party/blink/renderer/core/scheduler/dom_task_signal.h
// Upstream: third_party/blink/renderer/core/scheduler/dom_task_signal.cc

#ifndef MITTO_DOM_TASK_SIGNAL_H_
#define MITTO_DOM_TASK_SIGNAL_H_

#include <string>

#include "mitto/dom/abort_signal.h"

namespace mitto {

class TaskSignal final : public AbortSignal {
 public:
  // A signal of the default priority that follows nothing, as a Known Gap
  // gives one.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  // `priority` is one of TaskPriority's strings.
  TaskSignal(std::u16string priority, SignalType type);
  ~TaskSignal() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  std::u16string Priority() const { return priority_; }

 private:
  const std::u16string priority_;
};

}  // namespace mitto

#endif  // MITTO_DOM_TASK_SIGNAL_H_
