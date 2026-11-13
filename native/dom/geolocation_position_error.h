// Geolocation's GeolocationPositionError, which an error callback of
// navigator.geolocation gets.
//
// Upstream: third_party/blink/renderer/core/geolocation/geolocation_position_error.h

#ifndef MITTO_DOM_GEOLOCATION_POSITION_ERROR_H_
#define MITTO_DOM_GEOLOCATION_POSITION_ERROR_H_

#include <stdint.h>

#include <string>

#include "mitto/bindings/script_wrappable.h"

namespace mitto {

class GeolocationPositionError final : public bindings::ScriptWrappable {
 public:
  // The interface's constants.
  static constexpr uint16_t kPermissionDenied = 1;
  static constexpr uint16_t kTimeout = 3;

  static GeolocationPositionError* Create(uint16_t code,
                                          const std::u16string& message);

  GeolocationPositionError(uint16_t code, const std::u16string& message);
  ~GeolocationPositionError() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  uint16_t Code() const { return code_; }
  std::u16string Message() const { return message_; }

 private:
  const uint16_t code_;
  const std::u16string message_;
};

}  // namespace mitto

#endif  // MITTO_DOM_GEOLOCATION_POSITION_ERROR_H_
