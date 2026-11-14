#include "mitto/dom/idb_event_dispatcher.h"

#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/single_thread_task_runner.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/event.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/window.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-object.h"

namespace mitto {

DispatchEventResult DispatchIDBEvent(Event& event,
                                     const std::vector<EventTarget*>& targets) {
  CHECK(!targets.empty());
  // Held while listeners run, which the collector runs beside.
  std::vector<cppgc::Persistent<EventTarget>> path(targets.begin(),
                                                   targets.end());
  const cppgc::Persistent<Event> held(&event);

  // IDBEventDispatcher::Dispatch().
  event.SetPhase(Event::Phase::kCapturing);
  bool done = false;
  for (size_t i = path.size() - 1; i > 0; --i) {
    event.SetCurrentTarget(path[i].Get());
    path[i]->FireEventListeners(&event);
    if (event.PropagationStopped()) {
      done = true;
      break;
    }
  }

  if (!done) {
    event.SetPhase(Event::Phase::kAtTarget);
    event.SetCurrentTarget(path[0].Get());
    path[0]->FireEventListeners(&event);
    done =
        event.PropagationStopped() || !event.Bubbles() || event.CancelBubble();
  }

  if (!done) {
    event.SetPhase(Event::Phase::kBubbling);
    for (size_t i = 1; i < path.size(); ++i) {
      event.SetCurrentTarget(path[i].Get());
      path[i]->FireEventListeners(&event);
      if (event.PropagationStopped() || event.CancelBubble()) {
        break;
      }
    }
  }

  event.SetCurrentTarget(nullptr);
  event.SetPhase(Event::Phase::kNone);
  return EventTarget::GetDispatchEventResult(event);
}

Event* MakeIDBEvent(const std::u16string& type, bool bubbles, bool cancelable) {
  bindings::EventInit init;
  init.bubbles = bubbles;
  init.cancelable = cancelable;
  Event* const event = Event::Create(type, init);
  event->SetTrusted(true);
  return event;
}

bool RunInIDBContext(Window* window, base::FunctionRef<void()> run) {
  if (!window || !window->HasContext()) {
    return false;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::HandleScope handle_scope(isolate);
  // The window's wrapper is its context's global proxy.
  v8::Local<v8::Context> context;
  if (!window->ToV8(isolate)->GetCreationContext(isolate).ToLocal(&context)) {
    return false;
  }
  const v8::Context::Scope context_scope(context);
  // Holds back no microtasks: a listener's own scope is the outermost one
  // that runs them, as V8ScriptRunner::CallFunction()'s is in Blink.
  const v8::MicrotasksScope microtasks_scope(
      context, v8::MicrotasksScope::kDoNotRunMicrotasks);
  run();
  return true;
}

void PostIDBTask(Window* window, base::OnceClosure task) {
  // The Page program always makes one before it runs a script.
  EventLoop* const event_loop = EventLoop::Get();
  CHECK(event_loop);
  event_loop->TaskRunner(TaskType::kDatabaseAccess)
      ->PostTask(
          FROM_HERE,
          base::BindOnce(
              [](cppgc::WeakPersistent<Window> window, base::OnceClosure task) {
                RunInIDBContext(window.Get(), [&] { std::move(task).Run(); });
              },
              cppgc::WeakPersistent<Window>(window), std::move(task)));
}

}  // namespace mitto
