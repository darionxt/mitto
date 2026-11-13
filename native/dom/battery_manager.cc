#include "mitto/dom/battery_manager.h"

#include <limits>

#include "mitto/bindings/v8_battery_manager.h"

namespace mitto {

// static
BatteryManager* BatteryManager::Create() {
  return bindings::MakeGarbageCollected<BatteryManager>();
}

BatteryManager::BatteryManager() = default;
BatteryManager::~BatteryManager() = default;

const bindings::WrapperTypeInfo* BatteryManager::GetWrapperTypeInfo() const {
  return &bindings::V8BatteryManager::kWrapperTypeInfo;
}

double BatteryManager::DischargingTime() const {
  return std::numeric_limits<double>::infinity();
}

}  // namespace mitto
