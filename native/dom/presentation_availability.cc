#include "mitto/dom/presentation_availability.h"

#include "mitto/bindings/v8_presentation_availability.h"

namespace mitto {

// static
PresentationAvailability* PresentationAvailability::Create() {
  return bindings::MakeGarbageCollected<PresentationAvailability>();
}

PresentationAvailability::PresentationAvailability() = default;
PresentationAvailability::~PresentationAvailability() = default;

const bindings::WrapperTypeInfo* PresentationAvailability::GetWrapperTypeInfo()
    const {
  return &bindings::V8PresentationAvailability::kWrapperTypeInfo;
}

}  // namespace mitto
