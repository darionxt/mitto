// Performance Timeline's PerformanceObserver: what a page reads of it without
// observing, its static supportedEntryTypes, which follow the Page's features
// as Blink's do. Its constructor and its other members are Known Gaps.
//
// Upstream: third_party/blink/renderer/core/timing/performance_observer.cc

#ifndef MITTO_DOM_PERFORMANCE_OBSERVER_H_
#define MITTO_DOM_PERFORMANCE_OBSERVER_H_

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-value.h"

namespace mitto {

class PerformanceObserver final : public bindings::ScriptWrappable {
 public:
  static PerformanceObserver* Create();
  // The one a Known Gap gives, whose wrapper is of `info`.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  PerformanceObserver();
  ~PerformanceObserver() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  static v8::Local<v8::Value> SupportedEntryTypes();
};

}  // namespace mitto

#endif  // MITTO_DOM_PERFORMANCE_OBSERVER_H_
