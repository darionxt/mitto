// What Chrome's renderer sets up on the isolate of its main thread and on each
// context it makes there, beside V8's flags: the isolate's limits and the
// callbacks through which V8 asks Blink, where they change what a page sees.
//
// Upstream: gin/isolate_holder.cc
// Upstream: third_party/blink/renderer/platform/bindings/v8_per_isolate_data.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/v8_initializer.cc

#ifndef MITTO_PAGE_PAGE_ISOLATE_H_
#define MITTO_PAGE_PAGE_ISOLATE_H_

#include "v8/include/v8-forward.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

// Sets `params` as gin and Blink set them for a renderer's main thread: the
// heap's limits from `memory_gib`, the Machine's memory in GiB, where Chrome
// reads the machine's, and Atomics.wait() refused, as on every main thread.
void ConfigurePageIsolate(v8::Isolate::CreateParams& params, int memory_gib);

// Has `isolate` ask the Page what Blink answers for a renderer's main thread:
// whether a context has SharedArrayBuffer, what a DOMException is to
// Error.isError(), how big a module WebAssembly compiles there at once, whether
// it has WebAssembly's custom descriptors, and how
// WebAssembly.compileStreaming() reads a Response.
void InitializePageIsolate(v8::Isolate* isolate);

// What Blink sets on each context it makes: the clock of Temporal.Now,
// coarsened as performance.now() is for a context that is not cross-origin
// isolated.
void InitializePageContext(v8::Local<v8::Context> context);

}  // namespace mitto

#endif  // MITTO_PAGE_PAGE_ISOLATE_H_
