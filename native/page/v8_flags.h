// The V8 flags that a Page sets before V8 starts, as Chrome's renderer sets
// them. V8's flags are the process's, and each Page is a process of its own,
// so each Page can have flags of its own (docs/building.md, "V8's flags").
//
// Upstream: content/renderer/render_process_impl.cc
// Upstream: content/public/renderer/content_renderer_client.cc
// Upstream: gin/v8_initializer.cc

#ifndef MITTO_PAGE_V8_FLAGS_H_
#define MITTO_PAGE_V8_FLAGS_H_

#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"

namespace mitto {

// The flags that Chrome's own code sets in every renderer of the Engine Release
// on a machine with `memory_gib` of memory, in the order it sets them. What a
// variations seed's field trials set is a Persona's, and goes after these, so
// that one of them overrides one of these.
std::vector<std::string_view> EngineReleaseV8Flags(int memory_gib);

// Sets EngineReleaseV8Flags() and turns on V8's trap handler for WebAssembly,
// as Chrome's renderer does before V8 starts.
void SetEngineV8Flags(int memory_gib);

// The flags in `js_flags`, as Chrome's --js-flags= holds them: separated by
// commas, with the white space around each removed.
std::vector<std::string> SplitJsFlags(std::string_view js_flags);

// Sets each of `flags` on V8, in order, as Chrome's renderer does. A later
// flag overrides an earlier one. Called before V8 initialises.
void SetV8Flags(base::span<const std::string_view> flags);
void SetV8Flags(base::span<const std::string> flags);

}  // namespace mitto

#endif  // MITTO_PAGE_V8_FLAGS_H_
