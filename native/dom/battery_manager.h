// The Battery Status API's BatteryManager, which navigator.getBattery() gives:
// what Chrome's device service reads of a computer with no battery, which
// Windows reports as on AC power with no charge to give. The Machine claims a
// desktop, so its change events never fire.
//
// Upstream: third_party/blink/renderer/modules/battery/battery_manager.cc
// Upstream: third_party/blink/renderer/modules/battery/battery_status.h
// Upstream: services/device/public/mojom/battery_status.mojom
// Upstream: services/device/battery/battery_status_manager_win.cc

#ifndef MITTO_DOM_BATTERY_MANAGER_H_
#define MITTO_DOM_BATTERY_MANAGER_H_

#include "mitto/dom/event_target.h"

namespace mitto {

class BatteryManager final : public EventTarget {
 public:
  static BatteryManager* Create();

  BatteryManager();
  ~BatteryManager() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface: BatteryStatus's defaults.
  bool Charging() const { return true; }
  double ChargingTime() const { return 0; }
  double DischargingTime() const;
  double Level() const { return 1; }
};

}  // namespace mitto

#endif  // MITTO_DOM_BATTERY_MANAGER_H_
