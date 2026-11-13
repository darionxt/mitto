#include "mitto/dom/task_signal.h"

#include <utility>

#include "mitto/bindings/v8_task_signal.h"

namespace mitto {

// static
bindings::ScriptWrappable* TaskSignal::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  TaskSignal* const signal =
      static_cast<TaskSignal*>(bindings::MakeForKnownGap<TaskSignal>(
          info, bindings::V8TaskSignal::kWrapperTypeInfo, u"user-visible",
          SignalType::kComposite));
  signal->FollowSignals({});
  return signal;
}

TaskSignal::TaskSignal(std::u16string priority, SignalType type)
    : AbortSignal(type), priority_(std::move(priority)) {}

TaskSignal::~TaskSignal() = default;

const bindings::WrapperTypeInfo* TaskSignal::GetWrapperTypeInfo() const {
  return &bindings::V8TaskSignal::kWrapperTypeInfo;
}

}  // namespace mitto
