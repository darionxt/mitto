// WebAssembly.compileStreaming() and instantiateStreaming(), which V8 puts on
// WebAssembly only when its embedder tells it how to read a Response, as Blink
// does: the checks of a Response, with Blink's messages, and then its body,
// which V8 compiles.
//
// Upstream: third_party/blink/renderer/bindings/core/v8/v8_wasm_response_extensions.cc

#ifndef MITTO_PAGE_WASM_RESPONSE_EXTENSIONS_H_
#define MITTO_PAGE_WASM_RESPONSE_EXTENSIONS_H_

#include "v8/include/v8-forward.h"

namespace mitto {

// Gives `isolate` the callback, before its first context.
void InitializeWasmResponseExtensions(v8::Isolate* isolate);

}  // namespace mitto

#endif  // MITTO_PAGE_WASM_RESPONSE_EXTENSIONS_H_
