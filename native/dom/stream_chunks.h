// What the Runtime's own transformers share: the bytes of a chunk that a page
// wrote, an ArrayBuffer or a view, as Blink's V8BufferSource conversion takes
// them, and an ArrayBuffer or a Uint8Array of bytes to give a page.

#ifndef MITTO_DOM_STREAM_CHUNKS_H_
#define MITTO_DOM_STREAM_CHUNKS_H_

#include <stdint.h>

#include "base/containers/span.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-typed-array.h"

namespace v8 {
class Isolate;
class Value;
}  // namespace v8

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

// The bytes of `chunk`, valid until a script runs, or false after throwing
// Chrome's TypeError for one that is neither an ArrayBuffer nor a view.
bool BytesOfBufferSource(v8::Isolate* isolate,
                         v8::Local<v8::Value> chunk,
                         bindings::ExceptionState& exception_state,
                         base::span<const uint8_t>* bytes);

// A new ArrayBuffer of `bytes`.
v8::Local<v8::ArrayBuffer> NewArrayBuffer(v8::Isolate* isolate,
                                          base::span<const uint8_t> bytes);

// A new Uint8Array of `bytes`.
v8::Local<v8::Uint8Array> NewUint8Array(v8::Isolate* isolate,
                                        base::span<const uint8_t> bytes);

}  // namespace mitto

#endif  // MITTO_DOM_STREAM_CHUNKS_H_
