// What Blink keeps of how a window's document loaded: its DocumentLoader's
// DocumentLoadTiming, its Document's DocumentTiming, and the navigation's
// response with its load timing. performance.timing, the navigation's entry of
// the Performance Timeline and window.chrome all read it, so they agree.
//
// Upstream: third_party/blink/renderer/core/loader/document_load_timing.cc
// Upstream: third_party/blink/renderer/core/dom/document_timing.cc
// Upstream: third_party/blink/renderer/platform/loader/fetch/resource_load_timing.cc

#ifndef MITTO_DOM_DOCUMENT_LOAD_TIMING_H_
#define MITTO_DOM_DOCUMENT_LOAD_TIMING_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "base/time/time.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

// Whether a response came from the cache, as Blink's ResourceResponse works it
// out: kLocal for one that received nothing over the network, kValidated for
// one that the server validated.
enum class CacheState {
  kNone,
  kLocal,
  kValidated,
};

// //net's times of a response's last hop, as Blink's ResourceLoadTiming has
// them, each null for a step that did not happen.
struct ResourceLoadTiming {
  base::TimeTicks request_start;
  base::TimeTicks domain_lookup_start;
  base::TimeTicks domain_lookup_end;
  base::TimeTicks connect_start;
  base::TimeTicks connect_end;
  base::TimeTicks ssl_start;
  base::TimeTicks send_start;
  base::TimeTicks receive_headers_start;
  base::TimeTicks receive_headers_end;
  base::TimeTicks receive_non_informational_headers_start;
};

// A metric of a Server-Timing header, as Blink's ServerTimingHeader parses it.
struct ServerTimingInfo {
  std::u16string name;
  double duration = 0;
  std::u16string description;
};

// What an entry of the Performance Timeline for a resource is made from:
// Blink's mojom::ResourceTimingInfo, as CreateResourceTimingInfo() fills it.
struct ResourceTimingInfo {
  ResourceTimingInfo();
  ResourceTimingInfo(const ResourceTimingInfo&);
  ResourceTimingInfo& operator=(const ResourceTimingInfo&);
  ResourceTimingInfo(ResourceTimingInfo&&);
  ResourceTimingInfo& operator=(ResourceTimingInfo&&);
  ~ResourceTimingInfo();

  // The URL that the request started at.
  std::u16string name;
  // When the page asked for it, and when it ended.
  base::TimeTicks start_time;
  base::TimeTicks response_end;
  // Whether the response passed the TAO check, without which the timing of
  // its steps, its protocol and its sizes on the wire stay hidden.
  bool allow_timing_details = false;
  bool has_timing = false;
  ResourceLoadTiming timing;
  bool did_reuse_connection = false;
  bool is_secure_transport = false;
  std::u16string alpn_negotiated_protocol;
  std::u16string connection_info;
  CacheState cache_state = CacheState::kNone;
  base::TimeTicks last_redirect_end_time;
  uint64_t encoded_body_size = 0;
  uint64_t decoded_body_size = 0;
  // 0, and empty strings, where CORS hides them.
  uint16_t response_status = 0;
  std::u16string content_type;
  std::u16string content_encoding;
  std::vector<ServerTimingInfo> server_timing;
  bool render_blocking_status = false;
};

// One for each window. It reads as a time of the wall clock from the moment it
// starts, as DocumentLoadTiming takes its reference times.
class DocumentLoadTiming final
    : public cppgc::GarbageCollected<DocumentLoadTiming> {
 public:
  // Starts the times at `navigation_start`.
  explicit DocumentLoadTiming(base::TimeTicks navigation_start);
  ~DocumentLoadTiming();

  void Trace(cppgc::Visitor* visitor) const {}

  // PerformanceTiming::MonotonicTimeToIntegerMilliseconds(): `time` as a time
  // of the wall clock, coarsened as every time that a page reads is, in whole
  // milliseconds since the Unix epoch, or 0 for one that has not come.
  uint64_t ToIntegerMilliseconds(base::TimeTicks time) const;
  // DocumentLoadTiming::MonotonicTimeToPseudoWallTime(): `time` as a time of
  // the wall clock, since the Unix epoch, as precise as base::TimeTicks.
  base::TimeDelta MonotonicTimeToPseudoWallTime(base::TimeTicks time) const;

  // DocumentLoadTiming.
  base::TimeTicks navigation_start;
  base::TimeTicks fetch_start;
  base::TimeTicks redirect_start;
  base::TimeTicks redirect_end;
  uint16_t redirect_count = 0;
  bool has_cross_origin_redirect = false;
  base::TimeTicks response_end;
  base::TimeTicks load_event_start;
  base::TimeTicks load_event_end;
  base::TimeTicks critical_ch_restart;
  // Whether the navigation reloaded the document, which navigation timing
  // calls a reload.
  bool is_reload = false;
  // When the unload event of the document before this one started and ended,
  // which only a document of its origin sees, as Blink's
  // HasSameOriginAsPreviousDocument() says.
  base::TimeTicks unload_event_start;
  base::TimeTicks unload_event_end;
  bool has_same_origin_as_previous_document = false;
  // What the browser says of the navigation's confidence once the document is
  // interactive: whether it answered "high", at kRandomizedTriggerRate's
  // noise. Nothing before.
  std::optional<bool> high_confidence;

  // DocumentTiming.
  base::TimeTicks dom_loading;
  base::TimeTicks dom_interactive;
  base::TimeTicks dom_content_loaded_event_start;
  base::TimeTicks dom_content_loaded_event_end;
  base::TimeTicks dom_complete;

  // The navigation's response, which a window that the Session navigated has,
  // and about:blank's has not.
  bool has_response = false;
  // What the navigation's entry is made from.
  ResourceTimingInfo navigation;
  // Of its connection, as net::HttpResponseInfo has it, for window.chrome.
  std::string connection_info = "unknown";
  std::string alpn_negotiated_protocol = "unknown";
  bool was_fetched_via_spdy = false;
  bool was_alpn_negotiated = false;

 private:
  // DocumentLoadTiming's reference times: the wall clock's time at the zero of
  // base::TimeTicks.
  base::Time unix_at_zero_monotonic_;
};

}  // namespace mitto

#endif  // MITTO_DOM_DOCUMENT_LOAD_TIMING_H_
