// The runtime-enabled features a Page has, which decide what its bindings
// install, as Blink's RuntimeEnabledFeatures decide it for a renderer.
//
// A Page gets its set when it starts. Without one, it has the Engine Release's
// features for stable on Windows, which the bindings generator works out from
// runtime_enabled_features.json5. A set is given as the names to turn on and
// off from there, as Chrome's --enable-blink-features and
// --disable-blink-features give them, so that the Page's start message can
// carry the same set.
//
// Upstream: third_party/blink/renderer/build/scripts/templates/runtime_enabled_features.h.tmpl

#ifndef MITTO_BINDINGS_RUNTIME_FEATURE_SET_H_
#define MITTO_BINDINGS_RUNTIME_FEATURE_SET_H_

#include <bitset>
#include <optional>
#include <string>
#include <string_view>

#include "base/types/expected.h"
#include "mitto/bindings/runtime_features.h"

namespace mitto::bindings {

class RuntimeFeatureSet {
 public:
  // The Engine Release's features for stable on Windows.
  static RuntimeFeatureSet Stable();

  // The stable set with the features in `enable` turned on, and then those in
  // `disable` turned off, each a list separated by commas, as Chrome's
  // --enable-blink-features and --disable-blink-features give them. Returns the
  // first name that is not a feature as the error.
  static base::expected<RuntimeFeatureSet, std::string> FromSwitches(
      std::string_view enable,
      std::string_view disable);

  RuntimeFeatureSet(const RuntimeFeatureSet&);
  RuntimeFeatureSet& operator=(const RuntimeFeatureSet&);
  ~RuntimeFeatureSet();

  // Turns on, or off, each feature in `names`, which are separated by commas.
  // Returns the first name that is not a feature, and then changes nothing.
  std::optional<std::string> Enable(std::string_view names);
  std::optional<std::string> Disable(std::string_view names);

  // Whether `feature` is on: off if a feature it depends on is off, on if a
  // feature that implies it is on, and otherwise as the set has it.
  bool IsEnabled(RuntimeFeature feature) const;

 private:
  RuntimeFeatureSet();

  std::optional<std::string> Set(std::string_view names, bool enabled);

  std::bitset<kRuntimeFeatureCount> states_;
};

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_RUNTIME_FEATURE_SET_H_
