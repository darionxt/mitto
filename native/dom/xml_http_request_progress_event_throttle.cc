#include "mitto/dom/xml_http_request_progress_event_throttle.h"

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/time/time.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/event.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/progress_event.h"
#include "mitto/dom/window_task.h"
#include "mitto/dom/xml_http_request.h"
#include "v8/include/cppgc/persistent.h"

namespace mitto {
namespace {

// 50 ms per specification.
constexpr base::TimeDelta kMinimumProgressEventDispatchingInterval =
    base::Milliseconds(50);

}  // namespace

XMLHttpRequestProgressEventThrottle::XMLHttpRequestProgressEventThrottle(
    XMLHttpRequest* target)
    : target_(target) {}

XMLHttpRequestProgressEventThrottle::~XMLHttpRequestProgressEventThrottle() =
    default;

void XMLHttpRequestProgressEventThrottle::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(target_);
}

void XMLHttpRequestProgressEventThrottle::DispatchProgressEvent(
    const std::u16string& type,
    bool length_computable,
    uint64_t loaded,
    uint64_t total) {
  if (type != u"progress") {
    target_->DispatchEvent(
        ProgressEvent::CreateTrusted(type, length_computable, loaded, total));
    return;
  }
  if (active_) {
    deferred_ = DeferredEvent{length_computable, loaded, total};
    return;
  }
  DispatchProgressProgressEvent(
      ProgressEvent::CreateTrusted(type, length_computable, loaded, total));
  Start();
}

void XMLHttpRequestProgressEventThrottle::DispatchReadyStateChangeEvent(
    DeferredEventAction action) {
  const XMLHttpRequest::State state = target_->state();
  if (action == DeferredEventAction::kFlush) {
    if (deferred_) {
      DispatchProgressProgressEvent(TakeDeferred());
    }
    Stop();
  } else if (action == DeferredEventAction::kClear) {
    deferred_.reset();
    Stop();
  }

  has_dispatched_progress_progress_event_ = false;
  // A listener of the progress event that changed the state, as with abort(),
  // has had the readystatechange that it needed.
  if (state == target_->state()) {
    target_->DispatchReadyStateChange();
  }
}

void XMLHttpRequestProgressEventThrottle::Stop() {
  active_ = false;
  deferred_.reset();
}

void XMLHttpRequestProgressEventThrottle::DispatchProgressProgressEvent(
    Event* event) {
  const XMLHttpRequest::State state = target_->state();
  if (state == XMLHttpRequest::kLoading &&
      has_dispatched_progress_progress_event_) {
    target_->DispatchReadyStateChange();
  }
  if (target_->state() != state) {
    return;
  }
  has_dispatched_progress_progress_event_ = true;
  target_->DispatchEvent(event);
}

Event* XMLHttpRequestProgressEventThrottle::TakeDeferred() {
  DCHECK(deferred_);
  const DeferredEvent deferred = *deferred_;
  deferred_.reset();
  return ProgressEvent::CreateTrusted(u"progress", deferred.length_computable,
                                      deferred.loaded, deferred.total);
}

void XMLHttpRequestProgressEventThrottle::Start() {
  ExecutionContext* const context = target_->GetExecutionContext();
  if (!context) {
    return;
  }
  active_ = true;
  PostDelayedTaskInContext(
      context, TaskType::kNetworking,
      base::BindOnce(
          [](cppgc::WeakPersistent<XMLHttpRequestProgressEventThrottle>
                 throttle,
             uint64_t start) {
            if (throttle && throttle->active_ && throttle->start_ == start) {
              throttle->Fired();
            }
          },
          cppgc::WeakPersistent<XMLHttpRequestProgressEventThrottle>(this),
          ++start_),
      kMinimumProgressEventDispatchingInterval);
}

void XMLHttpRequestProgressEventThrottle::Fired() {
  active_ = false;
  // With no progress event since the last one, the timer stops.
  if (!deferred_) {
    return;
  }
  DispatchProgressProgressEvent(TakeDeferred());
  // Another one may come within the next 50 ms.
  Start();
}

}  // namespace mitto
