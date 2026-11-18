#include "mitto/dom/event_loop.h"

namespace mitto {
namespace {

// The Page's one event loop, on its main thread.
// Each thread that runs a Page's scripts has its own: the main thread's, and
// each dedicated worker's.
constinit thread_local EventLoop* g_event_loop = nullptr;

}  // namespace

// static
EventLoop* EventLoop::Get() {
  return g_event_loop;
}

// static
void EventLoop::Set(EventLoop* event_loop) {
  g_event_loop = event_loop;
}

}  // namespace mitto
