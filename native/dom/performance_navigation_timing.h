// Navigation Timing's PerformanceNavigationTiming: the entry of the window's
// navigation, which reads the window's DocumentLoadTiming as the document
// loads, as Blink's reads its document's and its loader's. The navigation's
// response always passes the TAO check, and is never restored from the
// back/forward cache or prerendered.
//
// Upstream: third_party/blink/renderer/core/timing/performance_navigation_timing.h
// Upstream: third_party/blink/renderer/core/timing/performance_navigation_timing.cc
// Upstream: third_party/blink/renderer/core/timing/performance_navigation_timing_activation_start.cc

#ifndef MITTO_DOM_PERFORMANCE_NAVIGATION_TIMING_H_
#define MITTO_DOM_PERFORMANCE_NAVIGATION_TIMING_H_

#include <stdint.h>

#include <string>

#include "mitto/dom/performance_resource_timing.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

class DocumentLoadTiming;
class PerformanceTimingConfidence;

class PerformanceNavigationTiming final : public PerformanceResourceTiming {
 public:
  PerformanceNavigationTiming(DocumentLoadTiming* timing,
                              base::TimeTicks time_origin,
                              uint64_t navigation_id);
  ~PerformanceNavigationTiming() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  double UnloadEventStart() const { return 0; }
  double UnloadEventEnd() const { return 0; }
  double DomInteractive() const;
  double DomContentLoadedEventStart() const;
  double DomContentLoadedEventEnd() const;
  double DomComplete() const;
  double LoadEventStart() const;
  double LoadEventEnd() const;
  std::u16string Type() const { return u"navigate"; }
  uint16_t RedirectCount() const;
  bindings::ScriptWrappable* NotRestoredReasons() const { return nullptr; }
  // A new object for each read, as Blink makes one, or null until the browser
  // has said.
  PerformanceTimingConfidence* Confidence() const;
  double CriticalCHRestart() const;
  double ActivationStart() const { return 0; }

  // PerformanceResourceTiming:
  std::u16string DeliveryType() const override;
  double RedirectStart() const override;
  double RedirectEnd() const override;
  double FetchStart() const override;
  double ResponseEnd() const override;
  // PerformanceEntry:
  double Duration() const override;
  Kind GetKind() const override;
  std::u16string_view TypeName() const override;

 protected:
  const ResourceTimingInfo& Info() const override;
  void BuildJSONValue(V8ObjectBuilder& builder) const override;

 private:
  // ComputeExposeCrossOriginRedirectTiming(): a chain of redirects that went
  // to another origin shows nothing of them, for a navigation with no referrer
  // has no TAO check for them.
  bool ExposesRedirectTiming() const;

  const cppgc::Member<DocumentLoadTiming> timing_;
};

}  // namespace mitto

#endif  // MITTO_DOM_PERFORMANCE_NAVIGATION_TIMING_H_
