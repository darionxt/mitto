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
    // gin/v8_initializer.cc sets nothing of its own: SetFeatureFlags() turns
    // only the features that the command line or a field trial overrides into
    // flags, and SetFlags() adds --use_strict for an isolate in strict mode,
    // which a renderer's is not.
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

void SetEngineV8Flags(int memory_gib) {
  SetV8Flags(EngineReleaseV8Flags(memory_gib));
  // RenderProcessImpl(): features::kWebAssemblyTrapHandler is on for 64-bit
  // desktops, and a guard region around WebAssembly's memory catches what
  // would otherwise be bounds checks. V8's own signal handler catches the
  // trap, as on Windows and macOS; on Linux Chrome's crash handler hands it to
  // V8 first, which comes to the same.
  v8::V8::EnableWebAssemblyTrapHandler(/*use_v8_signal_handler=*/true);
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
