// Fires an XMLHttpRequest's progress events at most each 50 ms, as Blink's
// XMLHttpRequestProgressEventThrottle: one that comes sooner waits, a later
// one takes its place, and the last one fires before the readystatechange of
// DONE, or never, if the request failed.
//
// Upstream: third_party/blink/renderer/core/xmlhttprequest/xml_http_request_progress_event_throttle.h
// Upstream: third_party/blink/renderer/core/xmlhttprequest/xml_http_request_progress_event_throttle.cc

#ifndef MITTO_DOM_XML_HTTP_REQUEST_PROGRESS_EVENT_THROTTLE_H_
#define MITTO_DOM_XML_HTTP_REQUEST_PROGRESS_EVENT_THROTTLE_H_

#include <stdint.h>

#include <optional>
#include <string>

#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class Event;
class XMLHttpRequest;

class XMLHttpRequestProgressEventThrottle final
    : public cppgc::GarbageCollected<XMLHttpRequestProgressEventThrottle> {
 public:
  // What a readystatechange does to the progress event that waits.
  enum class DeferredEventAction {
    kIgnore,
    kClear,
    kFlush,
  };

  explicit XMLHttpRequestProgressEventThrottle(XMLHttpRequest* target);
  XMLHttpRequestProgressEventThrottle(
      const XMLHttpRequestProgressEventThrottle&) = delete;
  XMLHttpRequestProgressEventThrottle& operator=(
      const XMLHttpRequestProgressEventThrottle&) = delete;
  ~XMLHttpRequestProgressEventThrottle();

  void Trace(cppgc::Visitor* visitor) const;

  // Fires an event of `type`, at once unless it is progress.
  void DispatchProgressEvent(const std::u16string& type,
                             bool length_computable,
                             uint64_t loaded,
                             uint64_t total);
  // Fires readystatechange, after the progress event that waits, if `action`
  // flushes it, and not if a listener of that one changes the state.
  void DispatchReadyStateChangeEvent(DeferredEventAction action);
  // Stops the timer. The progress event that waits never fires.
  void Stop();

 private:
  struct DeferredEvent {
    bool length_computable;
    uint64_t loaded;
    uint64_t total;
  };

  // Fires a progress event, after a readystatechange if it is not the first
  // of the state LOADING.
  void DispatchProgressProgressEvent(Event* event);
  Event* TakeDeferred();
  // Has Fired() run in 50 ms, unless Stop() comes first.
  void Start();
  void Fired();

  const cppgc::Member<XMLHttpRequest> target_;
  bool has_dispatched_progress_progress_event_ = false;
  std::optional<DeferredEvent> deferred_;
  // Whether the timer runs, and which start of it that is.
  bool active_ = false;
  uint64_t start_ = 0;
};

}  // namespace mitto

#endif  // MITTO_DOM_XML_HTTP_REQUEST_PROGRESS_EVENT_THROTTLE_H_
