#include "mitto/dom/readable_stream_byob_request.h"

#include <stddef.h>

#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_readable_stream_byob_request.h"
#include "mitto/dom/readable_byte_stream_controller.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

ReadableStreamBYOBRequest::ReadableStreamBYOBRequest(
    v8::Isolate* isolate,
    ReadableByteStreamController* controller,
    v8::Local<v8::Uint8Array> view)
    : controller_(controller), view_(isolate, view) {}

ReadableStreamBYOBRequest::~ReadableStreamBYOBRequest() = default;

const bindings::WrapperTypeInfo* ReadableStreamBYOBRequest::GetWrapperTypeInfo()
    const {
  return &bindings::V8ReadableStreamBYOBRequest::kWrapperTypeInfo;
}

void ReadableStreamBYOBRequest::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(controller_);
  visitor->Trace(view_);
  bindings::ScriptWrappable::Trace(visitor);
}

v8::Local<v8::ArrayBufferView> ReadableStreamBYOBRequest::View() const {
  // https://streams.spec.whatwg.org/#rs-byob-request-view
  // 1. Return this.[[view]].
  if (view_.IsEmpty()) {
    return {};
  }
  return view_.Get(v8::Isolate::GetCurrent());
}

void ReadableStreamBYOBRequest::Respond(
    uint64_t bytes_written,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#rs-byob-request-respond
  // 1. If this.[[controller]] is undefined, throw a TypeError exception.
  if (!controller_) {
    exception_state.ThrowTypeError(
        "Cannot respond to an invalidated ReadableStreamBYOBRequest");
    return;
  }
  v8::Isolate* const isolate = exception_state.GetIsolate();
  // 2. If ! IsDetachedBuffer(this.[[view]].[[ArrayBuffer]]) is true, throw a
  // TypeError exception.
  if (view_.Get(isolate)->Buffer()->WasDetached()) {
    exception_state.ThrowTypeError("ArrayBufferView is detached");
    return;
  }
  // 3. Assert: this.[[view]].[[ByteLength]] > 0.
  // 4. Assert: this.[[view]].[[ViewedArrayBuffer]].[[ByteLength]] > 0.
  // 5. Perform ? ReadableByteStreamControllerRespond(this.[[controller]],
  // bytesWritten).
  ReadableByteStreamController::Respond(isolate, controller_.Get(),
                                        static_cast<size_t>(bytes_written),
                                        exception_state);
}

void ReadableStreamBYOBRequest::RespondWithNewView(
    v8::Local<v8::ArrayBufferView> view,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#rs-byob-request-respond-with-new-view
  // 1. If this.[[controller]] is undefined, throw a TypeError exception.
  if (!controller_) {
    exception_state.ThrowTypeError(
        "Cannot respond to an invalidated ReadableStreamBYOBRequest");
    return;
  }
  // 2. If ! IsDetachedBuffer(view.[[ViewedArrayBuffer]]) is true, throw a
  // TypeError exception.
  if (view->Buffer()->WasDetached()) {
    exception_state.ThrowTypeError("ViewedArrayBuffer is detached");
    return;
  }
  // 3. Return ?
  // ReadableByteStreamControllerRespondWithNewView(this.[[controller]], view).
  ReadableByteStreamController::RespondWithNewView(
      exception_state.GetIsolate(), controller_.Get(), view, exception_state);
}

}  // namespace mitto
