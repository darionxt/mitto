#include "mitto/dom/performance_observer.h"

#include <string>
#include <vector>

#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/runtime_features.h"
#include "mitto/bindings/v8_performance_observer.h"
#include "mitto/dom/script_values.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

// static
PerformanceObserver* PerformanceObserver::Create() {
  return bindings::MakeGarbageCollected<PerformanceObserver>();
}

// static
bindings::ScriptWrappable* PerformanceObserver::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<PerformanceObserver>(
      info, bindings::V8PerformanceObserver::kWrapperTypeInfo);
}

PerformanceObserver::PerformanceObserver() = default;
PerformanceObserver::~PerformanceObserver() = default;

const bindings::WrapperTypeInfo* PerformanceObserver::GetWrapperTypeInfo()
    const {
  return &bindings::V8PerformanceObserver::kWrapperTypeInfo;
}

// Blink's supportedEntryTypes() for a window: the types that
// supportedEntryTypeMask() gives a window, in alphabetical order, with those
// behind a feature that the Page has.
// static
v8::Local<v8::Value> PerformanceObserver::SupportedEntryTypes() {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const bindings::RuntimeFeatureSet& features =
      bindings::PerIsolateData::From(isolate)->features();
  using bindings::RuntimeFeature;
  std::vector<std::u16string> types;
  if (features.IsEnabled(
          RuntimeFeature::kBackForwardCacheRestorationPerformanceEntry)) {
    types.push_back(u"back-forward-cache-restoration");
  }
  if (features.IsEnabled(RuntimeFeature::kContainerTiming)) {
    types.push_back(u"container");
  }
  for (const char16_t* type :
       {u"element", u"event", u"first-input", u"interaction-contentful-paint",
        u"largest-contentful-paint", u"layout-shift", u"long-animation-frame",
        u"longtask", u"mark", u"measure", u"navigation", u"paint",
        u"resource"}) {
    types.push_back(type);
  }
  if (features.IsEnabled(RuntimeFeature::kScrollPerformanceTiming)) {
    types.push_back(u"scroll");
  }
  types.push_back(u"soft-navigation");
  types.push_back(u"visibility-state");
  return FrozenArrayOfStrings(isolate, types);
}

}  // namespace mitto
