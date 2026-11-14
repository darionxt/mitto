#include "mitto/dom/performance_navigation_timing.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_performance_navigation_timing.h"
#include "mitto/dom/document_load_timing.h"
#include "mitto/dom/performance_timing_confidence.h"

namespace mitto {

PerformanceNavigationTiming::PerformanceNavigationTiming(
    DocumentLoadTiming* timing,
    base::TimeTicks time_origin,
    uint64_t navigation_id)
    : PerformanceResourceTiming(timing->navigation,
                                u"navigation",
                                time_origin,
                                navigation_id),
      timing_(timing) {}

PerformanceNavigationTiming::~PerformanceNavigationTiming() = default;

const bindings::WrapperTypeInfo*
PerformanceNavigationTiming::GetWrapperTypeInfo() const {
  return &bindings::V8PerformanceNavigationTiming::kWrapperTypeInfo;
}

void PerformanceNavigationTiming::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(timing_);
  PerformanceResourceTiming::Trace(visitor);
}

std::u16string PerformanceNavigationTiming::DeliveryType() const {
  return GetDeliveryType();
}

double PerformanceNavigationTiming::Duration() const {
  return LoadEventEnd();
}

PerformanceEntry::Kind PerformanceNavigationTiming::GetKind() const {
  return kNavigation;
}

std::u16string_view PerformanceNavigationTiming::TypeName() const {
  return u"navigation";
}

const ResourceTimingInfo& PerformanceNavigationTiming::Info() const {
  return timing_->navigation;
}

bool PerformanceNavigationTiming::ExposesRedirectTiming() const {
  return !timing_->has_cross_origin_redirect;
}

std::u16string PerformanceNavigationTiming::Type() const {
  return timing_->is_reload ? u"reload" : u"navigate";
}

// The old document's unload times, for a document of its origin that no
// redirect to another origin led to, as Blink gives them.
double PerformanceNavigationTiming::UnloadEventStart() const {
  if (timing_->has_cross_origin_redirect ||
      !timing_->has_same_origin_as_previous_document) {
    return 0;
  }
  return ToDOMHighResTimeStamp(timing_->unload_event_start);
}

double PerformanceNavigationTiming::UnloadEventEnd() const {
  if (timing_->has_cross_origin_redirect ||
      !timing_->has_same_origin_as_previous_document) {
    return 0;
  }
  return ToDOMHighResTimeStamp(timing_->unload_event_end);
}

double PerformanceNavigationTiming::DomInteractive() const {
  return ToDOMHighResTimeStamp(timing_->dom_interactive);
}

double PerformanceNavigationTiming::DomContentLoadedEventStart() const {
  return ToDOMHighResTimeStamp(timing_->dom_content_loaded_event_start);
}

double PerformanceNavigationTiming::DomContentLoadedEventEnd() const {
  return ToDOMHighResTimeStamp(timing_->dom_content_loaded_event_end);
}

double PerformanceNavigationTiming::DomComplete() const {
  return ToDOMHighResTimeStamp(timing_->dom_complete);
}

double PerformanceNavigationTiming::LoadEventStart() const {
  return ToDOMHighResTimeStamp(timing_->load_event_start);
}

double PerformanceNavigationTiming::LoadEventEnd() const {
  return ToDOMHighResTimeStamp(timing_->load_event_end);
}

uint16_t PerformanceNavigationTiming::RedirectCount() const {
  return ExposesRedirectTiming() ? timing_->redirect_count : 0;
}

double PerformanceNavigationTiming::RedirectStart() const {
  return ExposesRedirectTiming()
             ? ToDOMHighResTimeStamp(timing_->redirect_start)
             : 0;
}

double PerformanceNavigationTiming::RedirectEnd() const {
  return ExposesRedirectTiming() ? ToDOMHighResTimeStamp(timing_->redirect_end)
                                 : 0;
}

double PerformanceNavigationTiming::FetchStart() const {
  return ToDOMHighResTimeStamp(timing_->fetch_start);
}

double PerformanceNavigationTiming::ResponseEnd() const {
  return ToDOMHighResTimeStamp(timing_->response_end);
}

PerformanceTimingConfidence* PerformanceNavigationTiming::Confidence() const {
  if (!timing_->high_confidence) {
    return nullptr;
  }
  return bindings::MakeGarbageCollected<PerformanceTimingConfidence>(
      PerformanceTimingConfidence::kRandomizedTriggerRate,
      *timing_->high_confidence);
}

double PerformanceNavigationTiming::CriticalCHRestart() const {
  return ToDOMHighResTimeStamp(timing_->critical_ch_restart);
}

void PerformanceNavigationTiming::BuildJSONValue(
    V8ObjectBuilder& builder) const {
  PerformanceResourceTiming::BuildJSONValue(builder);
  builder.AddNumber("unloadEventStart", UnloadEventStart());
  builder.AddNumber("unloadEventEnd", UnloadEventEnd());
  builder.AddNumber("domInteractive", DomInteractive());
  builder.AddNumber("domContentLoadedEventStart", DomContentLoadedEventStart());
  builder.AddNumber("domContentLoadedEventEnd", DomContentLoadedEventEnd());
  builder.AddNumber("domComplete", DomComplete());
  builder.AddNumber("loadEventStart", LoadEventStart());
  builder.AddNumber("loadEventEnd", LoadEventEnd());
  builder.AddString("type", Type());
  builder.AddNumber("redirectCount", RedirectCount());
  builder.AddNumber("activationStart", ActivationStart());
  builder.AddNumber("criticalCHRestart", CriticalCHRestart());
  // BackForwardCacheNotRestoredReasons is stable, and the page was not
  // restored.
  builder.AddNull("notRestoredReasons");
  if (PerformanceTimingConfidence* const confidence = Confidence()) {
    builder.Add("confidence", bindings::ToV8(builder.isolate(), confidence));
  } else {
    builder.AddNull("confidence");
  }
}

}  // namespace mitto
