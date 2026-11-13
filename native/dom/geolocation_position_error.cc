#include "mitto/dom/geolocation_position_error.h"

#include "mitto/bindings/v8_geolocation_position_error.h"

namespace mitto {

// static
GeolocationPositionError* GeolocationPositionError::Create(
    uint16_t code,
    const std::u16string& message) {
  return bindings::MakeGarbageCollected<GeolocationPositionError>(code,
                                                                  message);
}

GeolocationPositionError::GeolocationPositionError(
    uint16_t code,
    const std::u16string& message)
    : code_(code), message_(message) {}

GeolocationPositionError::~GeolocationPositionError() = default;

const bindings::WrapperTypeInfo* GeolocationPositionError::GetWrapperTypeInfo()
    const {
  return &bindings::V8GeolocationPositionError::kWrapperTypeInfo;
}

}  // namespace mitto
