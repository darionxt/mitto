#include "mitto/page/v8_flags.h"

#include <stdint.h>

#include <array>

#include "base/strings/string_split.h"
#include "v8/include/v8-initialization.h"

namespace mitto {
namespace {

// The most memory of a low-end desktop, as base::SysInfo::IsLowEndDevice()
// counts it: features::kLowMemoryDeviceThresholdMB.
// Upstream: base/features.cc
constexpr int kLowEndDeviceMaxMemoryMib = 2048;

// What Chrome 157.0.8103.39's renderer sets, in its order. docs/building.md,
// "V8's flags", says where each comes from and what it changes.
//
// content/renderer/render_process_impl.cc, RenderProcessImpl(): first, on a
// low-end device.
constexpr std::string_view kOptimizeForSize = "--optimize-for-size";
constexpr auto kEngineReleaseV8Flags = std::to_array<std::string_view>({
    // RenderProcessImpl() again, after the flags of switches and of features
    // that only the command line or a field trial overrides: while
    // features::kSharedArrayBuffer is off, as by default, SharedArrayBuffer is
    // only for a context that Blink's callback allows, a cross-origin isolated
    // one, which a Page never is.
    "--enable-sharedarraybuffer-per-context",
    // gin/v8_initializer.cc, SetFeatureFlags(): each V8Flag_ feature that the
    // variations seed turns on, in the order of its name, then its parameters.
    // From seed 20261111-170049.393000-production, the groups that every
    // Baseline records under: V8DateTimePatternGeneratorCacheEviction
    // EnabledLaunch, V8SqrtAllocationLimits Enabled_80 and V8Turbolev Default.
    "--intl_date_time_pattern_generator_cache_eviction",
    "--maglev",
    "--sqrt_allocation_limits",
    "--sqrt_allocation_limits_factor=80",
    "--sqrt_allocation_limits_max_growing_factor=4",
    "--sqrt_allocation_limits_min_growing_factor=1.1",
    "--sqrt_allocation_limits_minimal_factor=20",
    "--sqrt_allocation_limits_slow_factor=80",
    "--turbofan",
    "--turbolev",
});

void SetV8Flag(std::string_view flag) {
  v8::V8::SetFlagsFromString(flag.data(), flag.size());
}

}  // namespace

std::vector<std::string_view> EngineReleaseV8Flags(int memory_gib) {
  std::vector<std::string_view> flags;
  if (static_cast<int64_t>(memory_gib) * 1024 <= kLowEndDeviceMaxMemoryMib) {
    flags.push_back(kOptimizeForSize);
  }
  flags.insert(flags.end(), kEngineReleaseV8Flags.begin(),
               kEngineReleaseV8Flags.end());
  return flags;
}

std::vector<std::string> SplitJsFlags(std::string_view js_flags) {
  return base::SplitString(js_flags, ",", base::TRIM_WHITESPACE,
                           base::SPLIT_WANT_NONEMPTY);
}

void SetV8Flags(base::span<const std::string_view> flags) {
  for (const std::string_view flag : flags) {
    SetV8Flag(flag);
  }
}

void SetV8Flags(base::span<const std::string> flags) {
  for (const std::string& flag : flags) {
    SetV8Flag(flag);
  }
}

}  // namespace mitto
