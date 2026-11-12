#include "mitto/dom/pressure_observer.h"

#include "mitto/bindings/v8_pressure_observer.h"
#include "mitto/dom/script_values.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

// static
PressureObserver* PressureObserver::Create() {
  return bindings::MakeGarbageCollected<PressureObserver>();
}

// static
bindings::ScriptWrappable* PressureObserver::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<PressureObserver>(
      info, bindings::V8PressureObserver::kWrapperTypeInfo);
}

PressureObserver::PressureObserver() = default;
PressureObserver::~PressureObserver() = default;

const bindings::WrapperTypeInfo* PressureObserver::GetWrapperTypeInfo() const {
  return &bindings::V8PressureObserver::kWrapperTypeInfo;
}

// static
v8::Local<v8::Value> PressureObserver::KnownSources() {
  return FrozenArrayOfStrings(v8::Isolate::GetCurrent(), {u"cpu"});
}

}  // namespace mitto
