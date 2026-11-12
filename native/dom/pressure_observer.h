// Compute Pressure's PressureObserver: what a page reads of it without
// observing, its static knownSources. Its constructor and other members are
// Known Gaps.
//
// Upstream: third_party/blink/renderer/modules/compute_pressure/pressure_observer.cc

#ifndef MITTO_DOM_PRESSURE_OBSERVER_H_
#define MITTO_DOM_PRESSURE_OBSERVER_H_

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-value.h"

namespace mitto {

class PressureObserver final : public bindings::ScriptWrappable {
 public:
  static PressureObserver* Create();
  // The one a Known Gap gives, whose wrapper is of `info`.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  PressureObserver();
  ~PressureObserver() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  static v8::Local<v8::Value> KnownSources();
};

}  // namespace mitto

#endif  // MITTO_DOM_PRESSURE_OBSERVER_H_
