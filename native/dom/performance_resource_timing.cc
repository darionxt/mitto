#include "mitto/dom/performance_resource_timing.h"

#include <algorithm>
#include <array>
#include <utility>

#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_performance_resource_timing.h"
#include "mitto/dom/performance.h"
#include "mitto/dom/performance_server_timing.h"
#include "mitto/dom/script_values.h"
#include "v8/include/v8-isolate.h"

namespace mitto {
namespace {

// The initiator types of the Resource Timing standard. Blink calls any other
// "other".
constexpr std::array<std::u16string_view, 21> kInitiatorTypeValues = {
    u"navigation",  u"body",  u"css",    u"script", u"xmlhttprequest",
    u"font",        u"fetch", u"beacon", u"video",  u"audio",
    u"track",       u"img",   u"image",  u"input",  u"ping",
    u"iframe",      u"frame", u"embed",  u"link",   u"object",
    u"early-hints",
};

std::u16string GetFilteredInitiatorType(std::u16string initiator_type) {
  if (initiator_type.empty() ||
      std::ranges::find(kInitiatorTypeValues, initiator_type) ==
          kInitiatorTypeValues.end()) {
    return u"other";
  }
  return initiator_type;
}

}  // namespace

PerformanceResourceTiming::PerformanceResourceTiming(
    ResourceTimingInfo info,
    std::u16string initiator_type,
    base::TimeTicks time_origin,
    uint64_t navigation_id)
    : PerformanceEntry(
          info.name,
          Performance::MonotonicTimeToDOMHighResTimeStamp(time_origin,
                                                          info.start_time),
          Performance::MonotonicTimeToDOMHighResTimeStamp(time_origin,
                                                          info.response_end),
          navigation_id),
      info_(std::move(info)),
      initiator_type_(GetFilteredInitiatorType(std::move(initiator_type))),
      time_origin_(time_origin) {
  for (const ServerTimingInfo& metric : info_.server_timing) {
    server_timing_.push_back(
        bindings::MakeGarbageCollected<PerformanceServerTiming>(metric));
  }
}

PerformanceResourceTiming::~PerformanceResourceTiming() = default;

const bindings::WrapperTypeInfo* PerformanceResourceTiming::GetWrapperTypeInfo()
    const {
  return &bindings::V8PerformanceResourceTiming::kWrapperTypeInfo;
}

void PerformanceResourceTiming::Trace(cppgc::Visitor* visitor) const {
  server_timing_.Trace(visitor);
  PerformanceEntry::Trace(visitor);
}

PerformanceEntry::Kind PerformanceResourceTiming::GetKind() const {
  return kResource;
}

std::u16string_view PerformanceResourceTiming::TypeName() const {
  return u"resource";
}

const ResourceTimingInfo& PerformanceResourceTiming::Info() const {
  return info_;
}

double PerformanceResourceTiming::ToDOMHighResTimeStamp(
    base::TimeTicks time) const {
  return Performance::MonotonicTimeToDOMHighResTimeStamp(time_origin_, time);
}

// static
uint64_t PerformanceResourceTiming::GetTransferSize(uint64_t encoded_body_size,
                                                    CacheState cache_state) {
  switch (cache_state) {
    case CacheState::kLocal:
      return 0;
    case CacheState::kValidated:
      return kHeaderSize;
    case CacheState::kNone:
      return encoded_body_size + kHeaderSize;
  }
}

std::u16string PerformanceResourceTiming::GetDeliveryType() const {
  return Info().cache_state == CacheState::kNone ? u"" : u"cache";
}

std::u16string PerformanceResourceTiming::DeliveryType() const {
  return Info().allow_timing_details ? GetDeliveryType() : u"";
}

std::u16string PerformanceResourceTiming::RenderBlockingStatus() const {
  return Info().render_blocking_status ? u"blocking" : u"non-blocking";
}

std::u16string PerformanceResourceTiming::NextHopProtocol() const {
  // ALPN's protocol, or the connection's when ALPN chose none; nothing when
  // neither is known or the TAO check failed.
  const ResourceTimingInfo& info = Info();
  std::u16string protocol = info.alpn_negotiated_protocol == u"unknown"
                                ? info.connection_info
                                : info.alpn_negotiated_protocol;
  if (protocol == u"unknown" || !info.allow_timing_details) {
    protocol.clear();
  }
  return protocol;
}

double PerformanceResourceTiming::RedirectStart() const {
  if (Info().last_redirect_end_time.is_null()) {
    return 0;
  }
  return StartTime();
}

double PerformanceResourceTiming::RedirectEnd() const {
  if (Info().last_redirect_end_time.is_null()) {
    return 0;
  }
  return ToDOMHighResTimeStamp(Info().last_redirect_end_time);
}

double PerformanceResourceTiming::FetchStart() const {
  const ResourceTimingInfo& info = Info();
  if (!info.has_timing) {
    return StartTime();
  }
  if (!info.last_redirect_end_time.is_null()) {
    return ToDOMHighResTimeStamp(info.timing.request_start);
  }
  return StartTime();
}

double PerformanceResourceTiming::DomainLookupStart() const {
  const ResourceTimingInfo& info = Info();
  if (!info.allow_timing_details) {
    return 0;
  }
  if (!info.has_timing || info.timing.domain_lookup_start.is_null()) {
    return FetchStart();
  }
  return ToDOMHighResTimeStamp(info.timing.domain_lookup_start);
}

double PerformanceResourceTiming::DomainLookupEnd() const {
  const ResourceTimingInfo& info = Info();
  if (!info.allow_timing_details) {
    return 0;
  }
  if (!info.has_timing || info.timing.domain_lookup_end.is_null()) {
    return DomainLookupStart();
  }
  return ToDOMHighResTimeStamp(info.timing.domain_lookup_end);
}

double PerformanceResourceTiming::ConnectStart() const {
  const ResourceTimingInfo& info = Info();
  if (!info.allow_timing_details) {
    return 0;
  }
  // No new connection, or none at all.
  if (!info.has_timing || info.timing.connect_start.is_null() ||
      info.did_reuse_connection) {
    return DomainLookupEnd();
  }
  // //net's connection includes its DNS lookup; Resource Timing's does not.
  base::TimeTicks connect_start = info.timing.connect_start;
  if (!info.timing.domain_lookup_end.is_null()) {
    connect_start = info.timing.domain_lookup_end;
  }
  return ToDOMHighResTimeStamp(connect_start);
}

double PerformanceResourceTiming::ConnectEnd() const {
  const ResourceTimingInfo& info = Info();
  if (!info.allow_timing_details) {
    return 0;
  }
  if (!info.has_timing || info.timing.connect_end.is_null() ||
      info.did_reuse_connection) {
    return ConnectStart();
  }
  return ToDOMHighResTimeStamp(info.timing.connect_end);
}

double PerformanceResourceTiming::SecureConnectionStart() const {
  const ResourceTimingInfo& info = Info();
  if (!info.allow_timing_details || !info.is_secure_transport) {
    return 0;
  }
  if (info.did_reuse_connection) {
    return FetchStart();
  }
  if (info.has_timing && !info.timing.ssl_start.is_null()) {
    return ToDOMHighResTimeStamp(info.timing.ssl_start);
  }
  return FetchStart();
}

double PerformanceResourceTiming::RequestStart() const {
  const ResourceTimingInfo& info = Info();
  if (!info.allow_timing_details) {
    return 0;
  }
  if (!info.has_timing || info.timing.send_start.is_null()) {
    return ConnectEnd();
  }
  return ToDOMHighResTimeStamp(info.timing.send_start);
}

double PerformanceResourceTiming::FirstInterimResponseStart() const {
  const ResourceTimingInfo& info = Info();
  if (!info.allow_timing_details || !info.has_timing) {
    return 0;
  }
  const base::TimeTicks response_start = info.timing.receive_headers_start;
  if (response_start.is_null() ||
      response_start == info.timing.receive_non_informational_headers_start) {
    return 0;
  }
  return ToDOMHighResTimeStamp(response_start);
}

double PerformanceResourceTiming::FinalResponseHeadersStart() const {
  const ResourceTimingInfo& info = Info();
  if (!info.allow_timing_details || !info.has_timing ||
      info.timing.receive_non_informational_headers_start.is_null()) {
    return 0;
  }
  return ToDOMHighResTimeStamp(
      info.timing.receive_non_informational_headers_start);
}

double PerformanceResourceTiming::ResponseStart() const {
  const ResourceTimingInfo& info = Info();
  if (!info.allow_timing_details) {
    return 0;
  }
  if (!info.has_timing) {
    return RequestStart();
  }
  base::TimeTicks response_start = info.timing.receive_headers_start;
  if (response_start.is_null()) {
    response_start = info.timing.receive_headers_end;
  }
  if (response_start.is_null()) {
    return RequestStart();
  }
  return ToDOMHighResTimeStamp(response_start);
}

double PerformanceResourceTiming::ResponseEnd() const {
  if (Info().response_end.is_null()) {
    return ResponseStart();
  }
  return ToDOMHighResTimeStamp(Info().response_end);
}

uint64_t PerformanceResourceTiming::TransferSize() const {
  if (!Info().allow_timing_details) {
    return 0;
  }
  return GetTransferSize(Info().encoded_body_size, Info().cache_state);
}

v8::Local<v8::Value> PerformanceResourceTiming::ServerTiming() const {
  // The bindings make a new frozen array of the same objects for each read.
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  std::vector<v8::Local<v8::Value>> items;
  for (PerformanceServerTiming* const metric : server_timing_.ToVector()) {
    items.push_back(bindings::ToV8(isolate, metric));
  }
  return FrozenArray(isolate, items);
}

v8::Local<v8::Value> PerformanceResourceTiming::ServerTimingSequence() const {
  return bindings::ToV8Array(v8::Isolate::GetCurrent(),
                             server_timing_.ToVector());
}

void PerformanceResourceTiming::BuildJSONValue(V8ObjectBuilder& builder) const {
  PerformanceEntry::BuildJSONValue(builder);
  builder.AddString("initiatorType", InitiatorType());
  builder.AddString("deliveryType", DeliveryType());
  builder.AddString("nextHopProtocol", NextHopProtocol());
  builder.AddString("renderBlockingStatus", RenderBlockingStatus());
  builder.AddString("contentType", ContentType());
  builder.AddString("contentEncoding", ContentEncoding());
  builder.AddNumber("workerStart", WorkerStart());
  // ServiceWorkerStaticRouterTimingInfo is stable.
  builder.AddNumber("workerRouterEvaluationStart",
                    WorkerRouterEvaluationStart());
  builder.AddNumber("workerCacheLookupStart", WorkerCacheLookupStart());
  builder.AddString("workerMatchedSourceType", WorkerMatchedSourceType());
  builder.AddString("workerFinalSourceType", WorkerFinalSourceType());
  builder.AddNumber("redirectStart", RedirectStart());
  builder.AddNumber("redirectEnd", RedirectEnd());
  builder.AddNumber("fetchStart", FetchStart());
  builder.AddNumber("domainLookupStart", DomainLookupStart());
  builder.AddNumber("domainLookupEnd", DomainLookupEnd());
  builder.AddNumber("connectStart", ConnectStart());
  builder.AddNumber("secureConnectionStart", SecureConnectionStart());
  builder.AddNumber("connectEnd", ConnectEnd());
  builder.AddNumber("requestStart", RequestStart());
  builder.AddNumber("responseStart", ResponseStart());
  builder.AddNumber("firstInterimResponseStart", FirstInterimResponseStart());
  builder.AddNumber("finalResponseHeadersStart", FinalResponseHeadersStart());
  builder.AddNumber("responseEnd", ResponseEnd());
  builder.AddNumber("transferSize", static_cast<double>(TransferSize()));
  builder.AddNumber("encodedBodySize", static_cast<double>(EncodedBodySize()));
  builder.AddNumber("decodedBodySize", static_cast<double>(DecodedBodySize()));
  builder.AddNumber("responseStatus", ResponseStatus());
  // toJSON() gives them as a sequence, which is not frozen.
  builder.Add("serverTiming", ServerTimingSequence());
}

}  // namespace mitto
