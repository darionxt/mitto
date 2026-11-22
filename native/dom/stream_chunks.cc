#include "mitto/dom/stream_chunks.h"

#include "base/compiler_specific.h"
#include "mitto/bindings/exception_messages.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_conversions.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

bool BytesOfBufferSource(v8::Isolate* isolate,
                         v8::Local<v8::Value> chunk,
                         bindings::ExceptionState& exception_state,
                         base::span<const uint8_t>* bytes) {
  // V8BufferSource's conversion, as bind_gen's union tries its members: a
  // SharedArrayBuffer goes to the ArrayBuffer member, which refuses it, and a
  // view on one is refused as shared.
  if (chunk->IsArrayBuffer() || chunk->IsSharedArrayBuffer()) {
    v8::Local<v8::ArrayBuffer> buffer;
    if (!bindings::FromV8ArrayBufferMember(chunk, exception_state, &buffer)) {
      return false;
    }
    if (!buffer->Data() || buffer->ByteLength() == 0) {
      *bytes = base::span<const uint8_t>();
      return true;
    }
    // SAFETY: the buffer holds ByteLength() bytes at Data().
    *bytes = UNSAFE_BUFFERS(base::span(
        static_cast<const uint8_t*>(buffer->Data()), buffer->ByteLength()));
    return true;
  }
  if (chunk->IsArrayBufferView()) {
    v8::Local<v8::ArrayBufferView> view;
    if (!bindings::FromV8ArrayBufferViewMember(chunk, exception_state,
                                               &view)) {
      return false;
    }
    if (view->ByteLength() == 0) {
      *bytes = base::span<const uint8_t>();
      return true;
    }
    const v8::Local<v8::ArrayBuffer> buffer = view->Buffer();
    // SAFETY: the view's bytes lie within its buffer.
    *bytes = UNSAFE_BUFFERS(
        base::span(static_cast<const uint8_t*>(buffer->Data()) +
                       view->ByteOffset(),
                   view->ByteLength()));
    return true;
  }
  exception_state.ThrowTypeError(bindings::ExceptionMessages::ValueNotOfType(
      "(ArrayBuffer or ArrayBufferView)"));
  return false;
}

v8::Local<v8::ArrayBuffer> NewArrayBuffer(v8::Isolate* isolate,
                                          base::span<const uint8_t> bytes) {
  const v8::Local<v8::ArrayBuffer> buffer =
      v8::ArrayBuffer::New(isolate, bytes.size());
  if (!bytes.empty()) {
    // SAFETY: the new buffer holds bytes.size() bytes.
    UNSAFE_BUFFERS(
        base::span(static_cast<uint8_t*>(buffer->Data()), bytes.size()))
        .copy_from(bytes);
  }
  return buffer;
}

v8::Local<v8::Uint8Array> NewUint8Array(v8::Isolate* isolate,
                                        base::span<const uint8_t> bytes) {
  return v8::Uint8Array::New(NewArrayBuffer(isolate, bytes), 0, bytes.size());
}

}  // namespace mitto
