#include "mitto/dom/performance_timing.h"

#include <array>
#include <string_view>
#include <utility>

#include "base/strings/string_util.h"
#include "mitto/bindings/v8_performance_timing.h"
#include "mitto/dom/document_load_timing.h"
#include "mitto/dom/performance_entry.h"
#include "v8/include/v8-isolate.h"

namespace mitto {
namespace {

using Getter = uint64_t (PerformanceTiming::*)() const;

// PerformanceTiming::GetAttributeMapping(), in the order a WTF HashMap of 64
// buckets keeps them, which toJSON() iterates: each name's StringHasher hash,
// rapidhash in its low 24 bits, with triangular probing, inserted in the
// order the IDL declares them.
// Upstream: third_party/blink/renderer/platform/wtf/hash_map.h
// Upstream: third_party/blink/renderer/platform/wtf/hash_table.h
// Upstream: third_party/blink/renderer/platform/wtf/text/string_hasher.h
constexpr std::array<std::pair<std::string_view, Getter>, 21> kAttributes = {{
    {"connectStart", &PerformanceTiming::ConnectStart},
    {"secureConnectionStart", &PerformanceTiming::SecureConnectionStart},
    {"unloadEventEnd", &PerformanceTiming::UnloadEventEnd},
    {"domainLookupStart", &PerformanceTiming::DomainLookupStart},
    {"domainLookupEnd", &PerformanceTiming::DomainLookupEnd},
    {"responseStart", &PerformanceTiming::ResponseStart},
    {"connectEnd", &PerformanceTiming::ConnectEnd},
    {"responseEnd", &PerformanceTiming::ResponseEnd},
    {"requestStart", &PerformanceTiming::RequestStart},
    {"domLoading", &PerformanceTiming::DomLoading},
    {"redirectStart", &PerformanceTiming::RedirectStart},
    {"loadEventEnd", &PerformanceTiming::LoadEventEnd},
    {"domComplete", &PerformanceTiming::DomComplete},
    {"navigationStart", &PerformanceTiming::NavigationStart},
    {"loadEventStart", &PerformanceTiming::LoadEventStart},
    {"domContentLoadedEventEnd", &PerformanceTiming::DomContentLoadedEventEnd},
    {"unloadEventStart", &PerformanceTiming::UnloadEventStart},
    {"redirectEnd", &PerformanceTiming::RedirectEnd},
    {"domInteractive", &PerformanceTiming::DomInteractive},
    {"fetchStart", &PerformanceTiming::FetchStart},
    {"domContentLoadedEventStart",
     &PerformanceTiming::DomContentLoadedEventStart},
}};

}  // namespace

PerformanceTiming::PerformanceTiming(DocumentLoadTiming* timing)
    : timing_(timing) {}

PerformanceTiming::~PerformanceTiming() = default;

const bindings::WrapperTypeInfo* PerformanceTiming::GetWrapperTypeInfo() const {
  return &bindings::V8PerformanceTiming::kWrapperTypeInfo;
}

void PerformanceTiming::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(timing_);
  bindings::ScriptWrappable::Trace(visitor);
}

uint64_t PerformanceTiming::ToIntegerMilliseconds(base::TimeTicks time) const {
  return timing_->ToIntegerMilliseconds(time);
}

uint64_t PerformanceTiming::NavigationStart() const {
  return ToIntegerMilliseconds(timing_->navigation_start);
}

// A Page's document never follows another from which it could request, as a
// new tab's first one does not.
uint64_t PerformanceTiming::UnloadEventStart() const {
  return timing_->has_same_origin_as_previous_document
             ? ToIntegerMilliseconds(timing_->unload_event_start)
             : 0;
}

uint64_t PerformanceTiming::UnloadEventEnd() const {
  return timing_->has_same_origin_as_previous_document
             ? ToIntegerMilliseconds(timing_->unload_event_end)
             : 0;
}

uint64_t PerformanceTiming::RedirectStart() const {
  if (timing_->has_cross_origin_redirect) {
    return 0;
  }
  return ToIntegerMilliseconds(timing_->redirect_start);
}

uint64_t PerformanceTiming::RedirectEnd() const {
  if (timing_->has_cross_origin_redirect) {
    return 0;
  }
  return ToIntegerMilliseconds(timing_->redirect_end);
}

uint64_t PerformanceTiming::FetchStart() const {
  return ToIntegerMilliseconds(timing_->fetch_start);
}

// The ResourceLoadTiming of the navigation's response, which about:blank's
// document has none of.
uint64_t PerformanceTiming::DomainLookupStart() const {
  if (!timing_->has_response ||
      timing_->navigation.timing.domain_lookup_start.is_null()) {
    return FetchStart();
  }
  return ToIntegerMilliseconds(timing_->navigation.timing.domain_lookup_start);
}

uint64_t PerformanceTiming::DomainLookupEnd() const {
  if (!timing_->has_response ||
      timing_->navigation.timing.domain_lookup_end.is_null()) {
    return DomainLookupStart();
  }
  return ToIntegerMilliseconds(timing_->navigation.timing.domain_lookup_end);
}

uint64_t PerformanceTiming::ConnectStart() const {
  const ResourceTimingInfo& navigation = timing_->navigation;
  if (!timing_->has_response || navigation.timing.connect_start.is_null() ||
      navigation.did_reuse_connection) {
    return DomainLookupEnd();
  }
  // Navigation Timing's connection does not include its DNS lookup.
  base::TimeTicks connect_start = navigation.timing.connect_start;
  if (!navigation.timing.domain_lookup_end.is_null() &&
      navigation.timing.domain_lookup_end > connect_start) {
    connect_start = navigation.timing.domain_lookup_end;
  }
  return ToIntegerMilliseconds(connect_start);
}

uint64_t PerformanceTiming::ConnectEnd() const {
  const ResourceTimingInfo& navigation = timing_->navigation;
  if (!timing_->has_response || navigation.timing.connect_end.is_null() ||
      navigation.did_reuse_connection) {
    return ConnectStart();
  }
  return ToIntegerMilliseconds(navigation.timing.connect_end);
}

uint64_t PerformanceTiming::SecureConnectionStart() const {
  if (!timing_->has_response) {
    return 0;
  }
  return ToIntegerMilliseconds(timing_->navigation.timing.ssl_start);
}

uint64_t PerformanceTiming::RequestStart() const {
  if (!timing_->has_response ||
      timing_->navigation.timing.send_start.is_null()) {
    return ConnectEnd();
  }
  return ToIntegerMilliseconds(timing_->navigation.timing.send_start);
}

uint64_t PerformanceTiming::ResponseStart() const {
  if (!timing_->has_response) {
    return RequestStart();
  }
  base::TimeTicks response_start =
      timing_->navigation.timing.receive_headers_start;
  if (response_start.is_null()) {
    response_start = timing_->navigation.timing.receive_headers_end;
  }
  if (response_start.is_null()) {
    return RequestStart();
  }
  return ToIntegerMilliseconds(response_start);
}

uint64_t PerformanceTiming::ResponseEnd() const {
  return ToIntegerMilliseconds(timing_->response_end);
}

uint64_t PerformanceTiming::DomLoading() const {
  return ToIntegerMilliseconds(timing_->dom_loading);
}

uint64_t PerformanceTiming::DomInteractive() const {
  return ToIntegerMilliseconds(timing_->dom_interactive);
}

uint64_t PerformanceTiming::DomContentLoadedEventStart() const {
  return ToIntegerMilliseconds(timing_->dom_content_loaded_event_start);
}

uint64_t PerformanceTiming::DomContentLoadedEventEnd() const {
  return ToIntegerMilliseconds(timing_->dom_content_loaded_event_end);
}

uint64_t PerformanceTiming::DomComplete() const {
  return ToIntegerMilliseconds(timing_->dom_complete);
}

uint64_t PerformanceTiming::LoadEventStart() const {
  return ToIntegerMilliseconds(timing_->load_event_start);
}

uint64_t PerformanceTiming::LoadEventEnd() const {
  return ToIntegerMilliseconds(timing_->load_event_end);
}

v8::Local<v8::Object> PerformanceTiming::ToJSONForBinding() const {
  V8ObjectBuilder builder(v8::Isolate::GetCurrent());
  for (const auto& [name, getter] : kAttributes) {
    builder.AddNumber(name, static_cast<double>((this->*getter)()));
  }
  return builder.Result();
}

// static
bool PerformanceTiming::IsAttributeName(std::u16string_view name) {
  for (const auto& [attribute, getter] : kAttributes) {
    if (base::EqualsASCII(name, attribute)) {
      return true;
    }
  }
  return false;
}

std::optional<uint64_t> PerformanceTiming::GetNamedAttribute(
    std::u16string_view name) const {
  for (const auto& [attribute, getter] : kAttributes) {
    if (base::EqualsASCII(name, attribute)) {
      return (this->*getter)();
    }
  }
  return std::nullopt;
}

}  // namespace mitto
