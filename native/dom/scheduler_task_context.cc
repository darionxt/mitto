#include "mitto/dom/scheduler_task_context.h"

#include "mitto/dom/abort_signal.h"
#include "mitto/dom/window.h"
#include "v8/include/v8-cpp-heap-external.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-sandbox.h"

namespace mitto {
namespace {

// The tag of the state's external, the first that V8 leaves the embedder for
// objects that wrap nothing, as Blink's kTaskAttributionTaskStateTag is
// Blink's first.
constexpr v8::CppHeapPointerTag kTag =
    v8::CppHeapPointerTag::kFirstEmbedderNonWrappableTag;

// TaskAttributionTaskState::SetCurrent(): undefined for none, which V8 handles
// fastest.
void SetCurrent(v8::Isolate* isolate, SchedulerTaskContext* context) {
  if (isolate->IsExecutionTerminating()) {
    return;
  }
  if (!context) {
    isolate->SetContinuationPreservedEmbedderData(v8::Undefined(isolate));
    return;
  }
  const v8::HandleScope handle_scope(isolate);
  isolate->SetContinuationPreservedEmbedderData(
      v8::CppHeapExternal::New(isolate, context, kTag));
}

}  // namespace

// static
SchedulerTaskContext* SchedulerTaskContext::Current(v8::Isolate* isolate) {
  if (isolate->IsExecutionTerminating()) {
    return nullptr;
  }
  const v8::HandleScope handle_scope(isolate);
  const v8::Local<v8::Data> data =
      isolate->GetContinuationPreservedEmbedderData();
  if (data->IsValue()) {
    return nullptr;
  }
  return v8::CppHeapExternal::Cast(*data)->Value<SchedulerTaskContext>(
      isolate, v8::CppHeapPointerTagRange(kTag, kTag));
}

// static
EventLoop::Queue SchedulerTaskContext::QueueFor(Priority priority,
                                                bool continuation) {
  switch (priority) {
    case Priority::kUserBlocking:
      return continuation ? EventLoop::Queue::kUserBlockingContinuation
                          : EventLoop::Queue::kUserBlocking;
    case Priority::kUserVisible:
      return continuation ? EventLoop::Queue::kUserVisibleContinuation
                          : EventLoop::Queue::kUserVisible;
    case Priority::kBackground:
      return continuation ? EventLoop::Queue::kBackgroundContinuation
                          : EventLoop::Queue::kBackground;
  }
}

SchedulerTaskContext::SchedulerTaskContext(Window* window,
                                           AbortSignal* abort_source,
                                           Priority priority)
    : window_(window), abort_source_(abort_source), priority_(priority) {}

void SchedulerTaskContext::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(window_);
  visitor->Trace(abort_source_);
}

bool SchedulerTaskContext::CanPropagateTo(const Window* window) const {
  return window_.Get() == window;
}

SchedulerTaskScope::SchedulerTaskScope(v8::Isolate* isolate,
                                       SchedulerTaskContext* context)
    : isolate_(isolate), previous_(SchedulerTaskContext::Current(isolate)) {
  if (context != previous_.Get()) {
    SetCurrent(isolate, context);
  }
}

SchedulerTaskScope::~SchedulerTaskScope() {
  SetCurrent(isolate_, previous_.Get());
}

}  // namespace mitto
