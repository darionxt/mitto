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
#include "mitto/dom/window_task.h"
#include "v8/include/cppgc/persistent.h"

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

void PostIDBTask(Window* window, base::OnceClosure task) {
  // The Page program always makes one before it runs a script.
  EventLoop* const event_loop = EventLoop::Get();
  CHECK(event_loop);
  event_loop->TaskRunner(TaskType::kDatabaseAccess)
      ->PostTask(
          FROM_HERE,
          base::BindOnce(
              [](cppgc::WeakPersistent<Window> window, base::OnceClosure task) {
                RunInWindowContext(window.Get(),
                                   [&] { std::move(task).Run(); });
              },
              cppgc::WeakPersistent<Window>(window), std::move(task)));
}

}  // namespace mitto
