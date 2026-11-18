#include "mitto/dom/readable_byte_stream_controller.h"

#include <stdint.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/numerics/checked_math.h"
#include "base/numerics/clamped_math.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_function.h"
#include "mitto/bindings/script_promise.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_readable_byte_stream_controller.h"
#include "mitto/dom/miscellaneous_operations.h"
#include "mitto/dom/read_request.h"
#include "mitto/dom/readable_stream.h"
#include "mitto/dom/readable_stream_byob_request.h"
#include "mitto/dom/readable_stream_default_reader.h"
#include "mitto/dom/readable_stream_generic_reader.h"
#include "mitto/dom/stream_algorithms.h"
#include "v8/include/v8-cppgc.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

namespace {

using ViewConstructor =
    v8::Local<v8::ArrayBufferView> (*)(v8::Local<v8::ArrayBuffer> buffer,
                                       size_t byte_offset,
                                       size_t length);

// The view constructor of a typed array kind, or of DataView, whose `length`
// is in bytes as its element size is 1.
template <typename ViewType>
v8::Local<v8::ArrayBufferView> CreateAsArrayBufferView(
    v8::Local<v8::ArrayBuffer> buffer,
    size_t byte_offset,
    size_t length) {
  return ViewType::New(buffer, byte_offset, length);
}

// The element size and the constructor that the typed array constructors
// table gives for `view`'s [[TypedArrayName]], for a view that is not a
// DataView.
void TypedArrayElementSizeAndConstructor(v8::Local<v8::ArrayBufferView> view,
                                         size_t* element_size,
                                         ViewConstructor* ctor) {
  if (view->IsInt8Array()) {
    *element_size = 1;
    *ctor = &CreateAsArrayBufferView<v8::Int8Array>;
  } else if (view->IsUint8Array()) {
    *element_size = 1;
    *ctor = &CreateAsArrayBufferView<v8::Uint8Array>;
  } else if (view->IsUint8ClampedArray()) {
    *element_size = 1;
    *ctor = &CreateAsArrayBufferView<v8::Uint8ClampedArray>;
  } else if (view->IsInt16Array()) {
    *element_size = 2;
    *ctor = &CreateAsArrayBufferView<v8::Int16Array>;
  } else if (view->IsUint16Array()) {
    *element_size = 2;
    *ctor = &CreateAsArrayBufferView<v8::Uint16Array>;
  } else if (view->IsInt32Array()) {
    *element_size = 4;
    *ctor = &CreateAsArrayBufferView<v8::Int32Array>;
  } else if (view->IsUint32Array()) {
    *element_size = 4;
    *ctor = &CreateAsArrayBufferView<v8::Uint32Array>;
  } else if (view->IsFloat16Array()) {
    *element_size = 2;
    *ctor = &CreateAsArrayBufferView<v8::Float16Array>;
  } else if (view->IsFloat32Array()) {
    *element_size = 4;
    *ctor = &CreateAsArrayBufferView<v8::Float32Array>;
  } else if (view->IsFloat64Array()) {
    *element_size = 8;
    *ctor = &CreateAsArrayBufferView<v8::Float64Array>;
  } else if (view->IsBigInt64Array()) {
    *element_size = 8;
    *ctor = &CreateAsArrayBufferView<v8::BigInt64Array>;
  } else {
    CHECK(view->IsBigUint64Array());
    *element_size = 8;
    *ctor = &CreateAsArrayBufferView<v8::BigUint64Array>;
  }
}

// The bytes of `buffer`.
base::span<uint8_t> ByteSpan(v8::Local<v8::ArrayBuffer> buffer) {
  // SAFETY: V8 gives the buffer's data and its length in bytes.
  return UNSAFE_BUFFERS(
      base::span(static_cast<uint8_t*>(buffer->Data()), buffer->ByteLength()));
}

}  // namespace

ReadableByteStreamController::QueueEntry::QueueEntry(
    v8::Isolate* isolate,
    v8::Local<v8::ArrayBuffer> buffer,
    size_t byte_offset,
    size_t byte_length)
    : buffer(isolate, buffer),
      byte_offset(byte_offset),
      byte_length(byte_length) {}

void ReadableByteStreamController::QueueEntry::Trace(
    cppgc::Visitor* visitor) const {
  visitor->Trace(buffer);
}

ReadableByteStreamController::PullIntoDescriptor::PullIntoDescriptor(
    v8::Isolate* isolate,
    v8::Local<v8::ArrayBuffer> buffer,
    size_t buffer_byte_length,
    size_t byte_offset,
    size_t byte_length,
    size_t bytes_filled,
    uint64_t minimum_fill,
    size_t element_size,
    ViewConstructorType view_constructor,
    ReaderType reader_type)
    : buffer(isolate, buffer),
      buffer_byte_length(buffer_byte_length),
      byte_offset(byte_offset),
      byte_length(byte_length),
      bytes_filled(bytes_filled),
      minimum_fill(minimum_fill),
      element_size(element_size),
      view_constructor(view_constructor),
      reader_type(reader_type) {}

void ReadableByteStreamController::PullIntoDescriptor::Trace(
    cppgc::Visitor* visitor) const {
  visitor->Trace(buffer);
}

// This constructor is used internally; it is not reachable from JavaScript.
ReadableByteStreamController::ReadableByteStreamController() = default;

ReadableByteStreamController::~ReadableByteStreamController() = default;

bool ReadableByteStreamController::IsByteStreamController() const {
  return true;
}

bool ReadableByteStreamController::IsDefaultController() const {
  return false;
}

const bindings::WrapperTypeInfo*
ReadableByteStreamController::GetWrapperTypeInfo() const {
  return &bindings::V8ReadableByteStreamController::kWrapperTypeInfo;
}

ReadableStreamBYOBRequest* ReadableByteStreamController::ByobRequest() {
  // https://streams.spec.whatwg.org/#rbs-controller-byob-request
  // 1. Return ReadableByteStreamControllerGetBYOBRequest(this).
  return GetBYOBRequest(this);
}

// static
ReadableStreamBYOBRequest* ReadableByteStreamController::GetBYOBRequest(
    ReadableByteStreamController* controller) {
  // https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontrollergetbyobrequest
  // 1. If controller.[[byobRequest]] is null and
  // controller.[[pendingPullIntos]] is not empty,
  if (!controller->byob_request_ && !controller->pending_pull_intos_.empty()) {
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    //   a. Let firstDescriptor be controller.[[pendingPullIntos]][0].
    const PullIntoDescriptor* first_descriptor =
        controller->pending_pull_intos_[0];

    //   b. Let view be ! Construct(%Uint8Array%, « firstDescriptor’s buffer,
    //   firstDescriptor’s byte offset + firstDescriptor’s bytes filled,
    //   firstDescriptor’s byte length − firstDescriptor’s bytes filled »).
    const v8::Local<v8::Uint8Array> view = v8::Uint8Array::New(
        first_descriptor->Buffer(isolate),
        first_descriptor->byte_offset + first_descriptor->bytes_filled,
        first_descriptor->byte_length - first_descriptor->bytes_filled);

    //   c. Let byobRequest be a new ReadableStreamBYOBRequest.
    //   d. Set byobRequest.[[controller]] to controller.
    //   e. Set byobRequest.[[view]] to view.
    //   f. Set controller.[[byobRequest]] to byobRequest.
    controller->byob_request_ =
        bindings::MakeGarbageCollected<ReadableStreamBYOBRequest>(
            isolate, controller, view);
  }

  // 2. Return controller.[[byobRequest]].
  return controller->byob_request_.Get();
}

std::optional<double> ReadableByteStreamController::DesiredSize() {
  // https://streams.spec.whatwg.org/#rbs-controller-desired-size
  // 1. Return ! ReadableByteStreamControllerGetDesiredSize(this).
  return GetDesiredSize(this);
}

// static
std::optional<double> ReadableByteStreamController::GetDesiredSize(
    ReadableByteStreamController* controller) {
  // https://streams.spec.whatwg.org/#readable-byte-stream-controller-get-desired-size
  // 1. Let state be controller.[[stream]].[[state]].
  switch (controller->controlled_readable_stream_->state_) {
      // 2. If state is "errored", return null.
    case ReadableStream::kErrored:
      return std::nullopt;

      // 3. If state is "closed", return 0.
    case ReadableStream::kClosed:
      return 0.0;

    case ReadableStream::kReadable:
      // 4. Return controller.[[strategyHWM]]] - controller.[[queueTotalSize]].
      return controller->strategy_high_water_mark_ -
             controller->queue_total_size_;
  }
}

void ReadableByteStreamController::Close(
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#rbs-controller-close
  // 1. If this.[[closeRequested]] is true, throw a TypeError exception.
  if (close_requested_) {
    exception_state.ThrowTypeError(
        "Cannot close a readable stream that has already been requested "
        "to be closed");
    return;
  }

  // 2. If this.[[stream]].[[state]] is not "readable", throw a TypeError
  // exception.
  if (controlled_readable_stream_->state_ != ReadableStream::kReadable) {
    exception_state.ThrowTypeError(
        "Cannot close a readable stream that is not readable");
    return;
  }

  // 3. Perform ? ReadableByteStreamControllerClose(this).
  Close(exception_state.GetIsolate(), this);
}

void ReadableByteStreamController::Enqueue(
    v8::Local<v8::ArrayBufferView> chunk,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#rbs-controller-enqueue
  // 1. If chunk.[[ByteLength]] is 0, throw a TypeError exception.
  if (chunk->ByteLength() == 0) {
    exception_state.ThrowTypeError("chunk is empty");
    return;
  }

  // 2. If chunk.[[ViewedArrayBuffer]].[[ArrayBufferByteLength]] is 0, throw a
  // TypeError exception.
  if (chunk->Buffer()->ByteLength() == 0) {
    exception_state.ThrowTypeError("chunk's buffer is empty");
    return;
  }

  // 3. If this.[[closeRequested]] is true, throw a TypeError exception.
  if (close_requested_) {
    exception_state.ThrowTypeError("close requested already");
    return;
  }

  // 4. If this.[[stream]].[[state]] is not "readable", throw a TypeError
  // exception.
  if (controlled_readable_stream_->state_ != ReadableStream::kReadable) {
    exception_state.ThrowTypeError("stream is not readable");
    return;
  }

  // 5. Return ! ReadableByteStreamControllerEnqueue(this, chunk).
  Enqueue(exception_state.GetIsolate(), this, chunk, exception_state);
}

void ReadableByteStreamController::Error(
    const std::optional<v8::Local<v8::Value>>& e) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  // https://streams.spec.whatwg.org/#rbs-controller-error
  // 1. Perform ! ReadableByteStreamControllerError(this, e).
  Error(isolate, this, e.value_or(v8::Undefined(isolate)));
}

// static
void ReadableByteStreamController::Close(
    v8::Isolate* isolate,
    ReadableByteStreamController* controller) {
  // https://streams.spec.whatwg.org/#readable-byte-stream-controller-close
  // 1. Let stream be controller.[[stream]].
  ReadableStream* const stream = controller->controlled_readable_stream_;

  // 2. If controller.[[closeRequested]] is true or stream.[[state]] is not
  // "readable", return.
  if (controller->close_requested_ ||
      stream->state_ != ReadableStream::kReadable) {
    return;
  }

  // 3. If controller.[[queueTotalSize]] > 0,
  if (controller->queue_total_size_ > 0) {
    //   a. Set controller.[[closeRequested]] to true.
    controller->close_requested_ = true;
    //   b. Return.
    return;
  }

  // 4. If controller.[[pendingPullIntos]] is not empty,
  if (!controller->pending_pull_intos_.empty()) {
    //  a. Let firstPendingPullInto be controller.[[pendingPullIntos]][0].
    const PullIntoDescriptor* first_pending_pull_into =
        controller->pending_pull_intos_[0];
    //  b. If the remainder after dividing firstPendingPullInto’s bytes filled
    //     by firstPendingPullInto's element size is not 0,
    //
    // The "min" option sets the minimum number of bytes a read should wait for
    // before resolving. Normally, reads only complete when at least `min` bytes
    // are available. However, if the stream closes before `min` bytes arrive,
    // it is allowed to resolve early with any data that forms complete elements
    // (no partial elements), even if fewer than `min`. This prevents the reader
    // from waiting indefinitely when the stream ends early, while still
    // ensuring data integrity by disallowing partial elements.
    if (first_pending_pull_into->bytes_filled %
            first_pending_pull_into->element_size !=
        0) {
      //     i. Let e be a new TypeError exception.
      const v8::Local<v8::Value> e =
          CreateTypeError(isolate, "Cannot close while responding");
      //     ii. Perform ! ReadableByteStreamControllerError(controller, e).
      Error(isolate, controller, e);
      //     iii. Throw e.
      isolate->ThrowException(e);
      return;
    }
  }

  // 5. Perform ! ReadableByteStreamControllerClearAlgorithms(controller).
  ClearAlgorithms(controller);

  // 6. Perform ! ReadableStreamClose(stream).
  ReadableStream::Close(isolate, stream);
}

// static
void ReadableByteStreamController::Error(
    v8::Isolate* isolate,
    ReadableByteStreamController* controller,
    v8::Local<v8::Value> e) {
  // https://streams.spec.whatwg.org/#readable-byte-stream-controller-error
  // 1. Let stream by controller.[[stream]].
  ReadableStream* const stream = controller->controlled_readable_stream_;

  // 2. If stream.[[state]] is not "readable", return.
  if (stream->state_ != ReadableStream::kReadable) {
    return;
  }

  // 3. Perform ! ReadableByteStreamControllerClearPendingPullIntos(controller).
  ClearPendingPullIntos(controller);

  // 4. Perform ! ResetQueue(controller).
  ResetQueue(controller);

  // 5. Perform ! ReadableByteStreamControllerClearAlgorithms(controller).
  ClearAlgorithms(controller);

  // 6. Perform ! ReadableStreamError(stream, e).
  ReadableStream::Error(isolate, stream, e);
}

// static
void ReadableByteStreamController::Enqueue(
    v8::Isolate* isolate,
    ReadableByteStreamController* controller,
    v8::Local<v8::ArrayBufferView> chunk,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#readable-byte-stream-controller-enqueue
  // 1. Let stream be controller.[[stream]].
  ReadableStream* const stream = controller->controlled_readable_stream_;

  // 2. If controller.[[closeRequested]] is true or stream.[[state]] is not
  // "readable", return.
  if (controller->close_requested_ ||
      stream->state_ != ReadableStream::kReadable) {
    return;
  }

  // 3. Let buffer be chunk.[[ViewedArrayBuffer]].
  const v8::Local<v8::ArrayBuffer> buffer = chunk->Buffer();

  // 4. Let byteOffset be chunk.[[ByteOffset]].
  const size_t byte_offset = chunk->ByteOffset();

  // 5. Let byteLength be chunk.[[ByteLength]].
  const size_t byte_length = chunk->ByteLength();

  // 6. If ! IsDetachedBuffer(buffer) is true, throw a TypeError exception.
  if (buffer->WasDetached()) {
    exception_state.ThrowTypeError("buffer is detached");
    return;
  }

  // 7. Let transferredBuffer be ? TransferArrayBuffer(buffer).
  const v8::Local<v8::ArrayBuffer> transferred_buffer =
      TransferArrayBuffer(isolate, buffer, exception_state);
  if (transferred_buffer.IsEmpty()) {
    return;
  }

  // 8. If controller.[[pendingPullIntos]] is not empty,
  if (!controller->pending_pull_intos_.empty()) {
    //     a. Let firstPendingPullInto be controller.[[pendingPullIntos]][0].
    PullIntoDescriptor* first_pending_pull_into =
        controller->pending_pull_intos_[0];
    //     b. If ! IsDetachedBuffer(firstPendingPullInto's buffer) is true,
    //     throw a TypeError exception.
    if (first_pending_pull_into->Buffer(isolate)->WasDetached()) {
      exception_state.ThrowTypeError("first pending read's buffer is detached");
      return;
    }
    //     c. Perform !
    //     ReadableByteStreamControllerInvalidateBYOBRequest(controller).
    InvalidateBYOBRequest(controller);
    //     d. Set firstPendingPullInto's buffer to ! TransferArrayBuffer(
    //     firstPendingPullInto's buffer).
    const v8::Local<v8::ArrayBuffer> transferred_pull_into_buffer =
        TransferArrayBuffer(isolate, first_pending_pull_into->Buffer(isolate),
                            exception_state);
    if (transferred_pull_into_buffer.IsEmpty()) {
      return;
    }
    first_pending_pull_into->SetBuffer(isolate, transferred_pull_into_buffer);
    //     e. If firstPendingPullInto’s reader type is "none", perform ?
    //     ReadableByteStreamControllerEnqueueDetachedPullIntoToQueue(
    //     controller, firstPendingPullInto).
    if (first_pending_pull_into->reader_type == ReaderType::kNone) {
      EnqueueDetachedPullIntoToQueue(isolate, controller,
                                     first_pending_pull_into);
    }
  }

  // 9. If ! ReadableStreamHasDefaultReader(stream) is true
  if (ReadableStream::HasDefaultReader(stream)) {
    //   a. Perform !
    //   ReadableByteStreamControllerProcessReadRequestsUsingQueue(controller).
    ProcessReadRequestsUsingQueue(isolate, controller, exception_state);
    //   b. If ! ReadableStreamGetNumReadRequests(stream) is 0,
    if (ReadableStream::GetNumReadRequests(stream) == 0) {
      //     i. Assert: controller.[[pendingPullIntos]] is empty.

      //     ii. Perform !
      //     ReadableByteStreamControllerEnqueueChunkToQueue(controller,
      //     transferredBuffer, byteOffset, byteLength).
      EnqueueChunkToQueue(isolate, controller, transferred_buffer, byte_offset,
                          byte_length);
    } else {
      // c. Otherwise,
      //     i. Assert: controller.[[queue]] is empty.

      //     ii. If controller.[[pendingPullIntos]] is not empty,
      if (!controller->pending_pull_intos_.empty()) {
        //        1. Assert: controller.[[pendingPullIntos]][0]'s reader type is
        //        "default".

        //        2. Perform !
        //        ReadableByteStreamControllerShiftPendingPullInto(controller).
        ShiftPendingPullInto(controller);
      }

      //     iii. Let transferredView be ! Construct(%Uint8Array%, «
      //     transferredBuffer, byteOffset, byteLength »).
      const v8::Local<v8::Value> transferred_view =
          v8::Uint8Array::New(transferred_buffer, byte_offset, byte_length);
      //     iv. Perform ! ReadableStreamFulfillReadRequest(stream,
      //     transferredView, false).
      ReadableStream::FulfillReadRequest(isolate, stream, transferred_view,
                                         false, exception_state);
      if (!exception_state.HadException()) {
        stream->DidConsumeBytes(byte_length);
      }
    }
  }

  // 10. Otherwise, if ! ReadableStreamHasBYOBReader(stream) is true,
  else if (ReadableStream::HasBYOBReader(stream)) {
    //   a. Perform !
    //   ReadableByteStreamControllerEnqueueChunkToQueue(controller,
    //   transferredBuffer, byteOffset, byteLength).
    EnqueueChunkToQueue(isolate, controller, transferred_buffer, byte_offset,
                        byte_length);
    //   b. Perform !
    //   ReadableByteStreamControllerProcessPullIntoDescriptorsUsing
    //   Queue(controller).
    ProcessPullIntoDescriptorsUsingQueue(isolate, controller);
  } else {
    // 11. Otherwise,
    //   a. Assert: ! IsReadableStreamLocked(stream) is false.

    //   b. Perform !
    //   ReadableByteStreamControllerEnqueueChunkToQueue(controller,
    //   transferredBuffer, byteOffset, byteLength).
    EnqueueChunkToQueue(isolate, controller, transferred_buffer, byte_offset,
                        byte_length);
  }

  // 12. Perform ! ReadableByteStreamControllerCallPullIfNeeded(controller).
  CallPullIfNeeded(isolate, controller);
}

// static
void ReadableByteStreamController::EnqueueChunkToQueue(
    v8::Isolate* isolate,
    ReadableByteStreamController* controller,
    v8::Local<v8::ArrayBuffer> buffer,
    size_t byte_offset,
    size_t byte_length) {
  // https://streams.spec.whatwg.org/#readable-byte-stream-controller-enqueue-chunk-to-queue
  // 1. Append a new readable byte stream queue entry with buffer buffer, byte
  // offset byteOffset, and byte length byteLength to controller.[[queue]].
  QueueEntry* const entry = bindings::MakeGarbageCollected<QueueEntry>(
      isolate, buffer, byte_offset, byte_length);
  controller->queue_.push_back(entry);
  // 2. Set controller.[[queueTotalSize]] to controller.[[queueTotalSize]] +
  // byteLength.
  controller->queue_total_size_ += byte_length;
}

// static
void ReadableByteStreamController::EnqueueClonedChunkToQueue(
    v8::Isolate* isolate,
    ReadableByteStreamController* controller,
    v8::Local<v8::ArrayBuffer> buffer,
    size_t byte_offset,
    size_t byte_length) {
  // https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontrollerenqueueclonedchunktoqueue
  // 1. Let cloneResult be CloneArrayBuffer(buffer, byteOffset, byteLength,
  // %ArrayBuffer%).
  const v8::Local<v8::ArrayBuffer> clone_result = v8::ArrayBuffer::New(
      isolate, byte_length, v8::BackingStoreInitializationMode::kUninitialized);
  ByteSpan(clone_result)
      .copy_from(ByteSpan(buffer).subspan(byte_offset, byte_length));
  // 2. If cloneResult is an abrupt completion,
  //   a. Perform ! ReadableByteStreamControllerError(controller,
  //   cloneResult.[[Value]]). b. Return cloneResult.
  // This is not needed as v8::ArrayBuffer::New() crashes if it cannot allocate
  // the memory.

  // 3. Perform ! ReadableByteStreamControllerEnqueueChunkToQueue(controller,
  // cloneResult.[[Value]], 0, byteLength).
  EnqueueChunkToQueue(isolate, controller, clone_result, 0, byte_length);
}

// static
void ReadableByteStreamController::EnqueueDetachedPullIntoToQueue(
    v8::Isolate* isolate,
    ReadableByteStreamController* controller,
    PullIntoDescriptor* pull_into_descriptor) {
  // https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontrollerenqueuedetachedpullintotoqueue
  // Note: EnqueueDetachedPullIntoToQueue cannot throw in this implementation.
  // 1. Assert: pullIntoDescriptor’s reader type is "none".
  CHECK(pull_into_descriptor->reader_type == ReaderType::kNone);

  // 2. If pullIntoDescriptor’s bytes filled > 0, perform ?
  // ReadableByteStreamControllerEnqueueClonedChunkToQueue(controller,
  // pullIntoDescriptor’s buffer, pullIntoDescriptor’s byte offset,
  // pullIntoDescriptor’s bytes filled).
  if (pull_into_descriptor->bytes_filled > 0) {
    EnqueueClonedChunkToQueue(
        isolate, controller, pull_into_descriptor->Buffer(isolate),
        pull_into_descriptor->byte_offset, pull_into_descriptor->bytes_filled);
  }

  // 3. Perform ! ReadableByteStreamControllerShiftPendingPullInto(controller).
  ShiftPendingPullInto(controller);
}

// static
void ReadableByteStreamController::ProcessPullIntoDescriptorsUsingQueue(
    v8::Isolate* isolate,
    ReadableByteStreamController* controller) {
  // https://streams.spec.whatwg.org/#readable-byte-stream-controller-process-pull-into-descriptors-using-queue
  // 1. Assert: controller.[[closeRequested]] is false.
  const v8::TryCatch try_catch(isolate);
  // 2. While controller.[[pendingPullIntos]] is not empty,
  while (!controller->pending_pull_intos_.empty()) {
    //   a. If controller.[[queueTotalSize]] is 0, return.
    if (controller->queue_total_size_ == 0) {
      return;
    }
    //   b. Let pullIntoDescriptor be controller.[[pendingPullIntos]][0].
    PullIntoDescriptor* const pull_into_descriptor =
        controller->pending_pull_intos_[0];
    //   c. If ! ReadableByteStreamControllerFillPullIntoDescriptorFromQueue(
    //   controller, pullIntoDescriptor) is true,
    bindings::ExceptionState fill_exception_state(isolate);
    if (FillPullIntoDescriptorFromQueue(
            isolate, controller, pull_into_descriptor, fill_exception_state)) {
      //     i. Perform !
      //     ReadableByteStreamControllerShiftPendingPullInto(controller).
      ShiftPendingPullInto(controller);
      //     ii. Perform ! ReadableByteStreamControllerCommitPullIntoDescriptor(
      //     controller.[[stream]], pullIntoDescriptor).
      bindings::ExceptionState commit_exception_state(isolate);
      CommitPullIntoDescriptor(isolate, controller->controlled_readable_stream_,
                               pull_into_descriptor, commit_exception_state);
    }
    if (try_catch.HasCaught()) {
      // Instead of returning a rejection, which is inconvenient here,
      // call ControllerError(). The only difference this makes is that it
      // happens synchronously, but that should not be observable.
      ReadableByteStreamController::Error(isolate, controller,
                                          try_catch.Exception());
      return;
    }
  }
}

// static
void ReadableByteStreamController::ProcessReadRequestsUsingQueue(
    v8::Isolate* isolate,
    ReadableByteStreamController* controller,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontrollerprocessreadrequestsusingqueue
  // 1. Let reader be controller.[[stream]].[[reader]].
  ReadableStreamGenericReader* reader =
      controller->controlled_readable_stream_->reader_;
  // 2. Assert: reader implements ReadableStreamDefaultReader.
  CHECK(reader->IsDefaultReader());
  ReadableStreamDefaultReader* default_reader =
      static_cast<ReadableStreamDefaultReader*>(reader);
  // 3. While reader.[[readRequests]] is not empty,
  while (!default_reader->read_requests_.empty()) {
    //   a. If controller.[[queueTotalSize]] is 0, return.
    if (controller->queue_total_size_ == 0) {
      return;
    }
    //   b. Let readRequest be reader.[[readRequests]][0].
    ReadRequest* read_request = default_reader->read_requests_[0];
    //   c. Remove readRequest from reader.[[readRequests]].
    default_reader->read_requests_.pop_front();
    //   d. Perform !
    //   ReadableByteStreamControllerFillReadRequestFromQueue(controller,
    //   readRequest).
    FillReadRequestFromQueue(isolate, controller, read_request,
                             exception_state);
  }
}

// static
void ReadableByteStreamController::CallPullIfNeeded(
    v8::Isolate* isolate,
    ReadableByteStreamController* controller) {
  // https://streams.spec.whatwg.org/#readable-byte-stream-controller-call-pull-if-needed
  // 1. Let shouldPull be !
  // ReadableByteStreamControllerShouldCallPull(controller).
  const bool should_pull = ShouldCallPull(controller);
  // 2. If shouldPull is false, return.
  if (!should_pull) {
    return;
  }
  // 3. If controller.[[pulling]] is true,
  if (controller->pulling_) {
    //   a. Set controller.[[pullAgain]] to true.
    controller->pull_again_ = true;
    //   b. Return.
    return;
  }
  // 4. Assert: controller.[[pullAgain]] is false.
  // 5. Set controller.[[pulling]] to true.
  controller->pulling_ = true;
  // 6. Let pullPromise be the result of performing
  // controller.[[pullAlgorithm]].
  const v8::Local<v8::Promise> pull_promise =
      controller->pull_algorithm_->Run(isolate, {});

  class ResolveFunction final : public bindings::ScriptFunction {
   public:
    explicit ResolveFunction(ReadableByteStreamController* controller)
        : controller_(controller) {}

    v8::Local<v8::Value> Call(v8::Isolate* isolate,
                              v8::Local<v8::Value>) override {
      // 7. Upon fulfillment of pullPromise,
      //   a. Set controller.[[pulling]] to false.
      controller_->pulling_ = false;
      //   b. If controller.[[pullAgain]] is true,
      if (controller_->pull_again_) {
        //     i. Set controller.[[pullAgain]] to false.
        controller_->pull_again_ = false;
        //     ii. Perform !
        //     ReadableByteStreamControllerCallPullIfNeeded(controller).
        CallPullIfNeeded(isolate, controller_);
      }
      return v8::Undefined(isolate);
    }

    void Trace(cppgc::Visitor* visitor) const override {
      visitor->Trace(controller_);
      bindings::ScriptFunction::Trace(visitor);
    }

   private:
    const cppgc::Member<ReadableByteStreamController> controller_;
  };

  class RejectFunction final : public bindings::ScriptFunction {
   public:
    explicit RejectFunction(ReadableByteStreamController* controller)
        : controller_(controller) {}

    v8::Local<v8::Value> Call(v8::Isolate* isolate,
                              v8::Local<v8::Value> e) override {
      // 8. Upon rejection of pullPromise with reason e,
      //   a. Perform ! ReadableByteStreamControllerError(controller, e).
      Error(isolate, controller_, e);
      return v8::Undefined(isolate);
    }

    void Trace(cppgc::Visitor* visitor) const override {
      visitor->Trace(controller_);
      bindings::ScriptFunction::Trace(visitor);
    }

   private:
    const cppgc::Member<ReadableByteStreamController> controller_;
  };

  bindings::PromiseThen(
      isolate, pull_promise,
      bindings::MakeGarbageCollected<ResolveFunction>(controller),
      bindings::MakeGarbageCollected<RejectFunction>(controller));
}

// static
ReadableByteStreamController::PullIntoDescriptor*
ReadableByteStreamController::ShiftPendingPullInto(
    ReadableByteStreamController* controller) {
  // https://streams.spec.whatwg.org/#readable-byte-stream-controller-shift-pending-pull-into
  // 1. Assert: controller.[[byobRequest]] is null.
  // 2. Let descriptor be controller.[[pendingPullIntos]][0].
  PullIntoDescriptor* const descriptor = controller->pending_pull_intos_[0];
  // 3. Remove descriptor from controller.[[pendingPullIntos]].
  controller->pending_pull_intos_.pop_front();
  // 4. Return descriptor.
  return descriptor;
}

// static
bool ReadableByteStreamController::ShouldCallPull(
    ReadableByteStreamController* controller) {
  // https://streams.spec.whatwg.org/#readable-byte-stream-controller-should-call-pull
  // 1. Let stream be controller.[[stream]].
  ReadableStream* const stream = controller->controlled_readable_stream_;
  // 2. If stream.[[state]] is not "readable", return false.
  if (stream->state_ != ReadableStream::kReadable) {
    return false;
  }
  // 3. If controller.[[closeRequested]] is true, return false.
  if (controller->close_requested_) {
    return false;
  }
  // 4. If controller.[[started]] is false, return false.
  if (!controller->started_) {
    return false;
  }
  // 5. If ! ReadableStreamHasDefaultReader(stream) is true and !
  // ReadableStreamGetNumReadRequests(stream) > 0, return true.
  if (ReadableStream::HasDefaultReader(stream) &&
      ReadableStream::GetNumReadRequests(stream) > 0) {
    return true;
  }
  // 6. If ! ReadableStreamHasBYOBReader(stream) is true and !
  // ReadableStreamGetNumReadIntoRequests(stream) > 0, return true.
  if (ReadableStream::HasBYOBReader(stream) &&
      ReadableStream::GetNumReadIntoRequests(stream) > 0) {
    return true;
  }
  // 7. Let desiredSize be !
  // ReadableByteStreamControllerGetDesiredSize(controller).
  const std::optional<double> desired_size = GetDesiredSize(controller);
  // 8. Assert: desiredSize is not null.
  CHECK(desired_size);
  // 9. If desiredSize > 0, return true.
  if (*desired_size > 0) {
    return true;
  }
  // 10. Return false.
  return false;
}

// static
void ReadableByteStreamController::CommitPullIntoDescriptor(
    v8::Isolate* isolate,
    ReadableStream* stream,
    PullIntoDescriptor* pull_into_descriptor,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#readable-byte-stream-controller-commit-pull-into-descriptor
  // 1. Assert: stream.[[state]] is not "errored".
  // 2. Assert: pullIntoDescriptor.reader type is not "none".
  // 3. Let done be false.
  bool done = false;
  // 4. If stream.[[state]] is "closed",
  if (stream->state_ == ReadableStream::kClosed) {
    //   a. Assert: the remainder after dividing pullIntoDescriptor’s
    //      bytes filled by pullIntoDescriptor’s element size is 0.
    CHECK_EQ(
        pull_into_descriptor->bytes_filled % pull_into_descriptor->element_size,
        0u);
    //   b. Set done to true.
    done = true;
  }
  // 5. Let filledView be !
  // ReadableByteStreamControllerConvertPullIntoDescriptor(pullIntoDescriptor).
  const v8::Local<v8::ArrayBufferView> filled_view =
      ConvertPullIntoDescriptor(isolate, pull_into_descriptor, exception_state);
  if (filled_view.IsEmpty()) {
    return;
  }
  // 6. If pullIntoDescriptor’s reader type is "default",
  if (pull_into_descriptor->reader_type == ReaderType::kDefault) {
    //   a. Perform ! ReadableStreamFulfillReadRequest(stream, filledView,
    //   done).
    ReadableStream::FulfillReadRequest(isolate, stream, filled_view, done,
                                       exception_state);
  } else {
    // 7. Otherwise,
    //   a. Assert: pullIntoDescriptor’s reader type is "byob".
    //   b. Perform ! ReadableStreamFulfillReadIntoRequest(stream, filledView,
    //   done).
    ReadableStream::FulfillReadIntoRequest(isolate, stream, filled_view, done,
                                           exception_state);
  }
  if (!exception_state.HadException()) {
    stream->DidConsumeBytes(filled_view->ByteLength());
  }
}

// static
v8::Local<v8::ArrayBufferView>
ReadableByteStreamController::ConvertPullIntoDescriptor(
    v8::Isolate* isolate,
    PullIntoDescriptor* pull_into_descriptor,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#readable-byte-stream-controller-convert-pull-into-descriptor
  // 1. Let bytesFilled be pullIntoDescriptor’s bytes filled.
  const size_t bytes_filled = pull_into_descriptor->bytes_filled;
  // 2. Let elementSize be pullIntoDescriptor’s element size.
  const size_t element_size = pull_into_descriptor->element_size;
  // 3. Assert: bytesFilled ≤ pullIntoDescriptor’s byte length.
  // 4. Assert: bytesFilled mod elementSize is 0.
  // 5. Let buffer be ! TransferArrayBuffer(pullIntoDescriptor's buffer).
  const v8::Local<v8::ArrayBuffer> buffer = TransferArrayBuffer(
      isolate, pull_into_descriptor->Buffer(isolate), exception_state);
  if (buffer.IsEmpty()) {
    return {};
  }
  // 6. Return ! Construct(pullIntoDescriptor’s view constructor, « buffer,
  // pullIntoDescriptor’s byte offset, bytesFilled ÷ elementSize »).
  return pull_into_descriptor->view_constructor(
      buffer, pull_into_descriptor->byte_offset, (bytes_filled / element_size));
}

// static
void ReadableByteStreamController::ClearPendingPullIntos(
    ReadableByteStreamController* controller) {
  // https://streams.spec.whatwg.org/#readable-byte-stream-controller-clear-pending-pull-intos
  // 1. Perform ! ReadableByteStreamControllerInvalidateBYOBRequest(controller).
  InvalidateBYOBRequest(controller);
  // 2. Set controller.[[pendingPullIntos]] to a new empty list.
  controller->pending_pull_intos_.clear();
}

// static
void ReadableByteStreamController::ClearAlgorithms(
    ReadableByteStreamController* controller) {
  // https://streams.spec.whatwg.org/#readable-byte-stream-controller-clear-algorithms
  // 1. Set controller.[[pullAlgorithm]] to undefined.
  controller->pull_algorithm_ = nullptr;

  // 2. Set controller.[[cancelAlgorithm]] to undefined.
  controller->cancel_algorithm_ = nullptr;
}

// static
void ReadableByteStreamController::InvalidateBYOBRequest(
    ReadableByteStreamController* controller) {
  // https://streams.spec.whatwg.org/#readable-byte-stream-controller-invalidate-byob-request
  // 1. If controller.[[byobRequest]] is null, return.
  if (!controller->byob_request_) {
    return;
  }
  // 2. Set controller.[[byobRequest]].[[controller]] to undefined.
  controller->byob_request_->controller_ = nullptr;
  // 3. Set controller.[[byobRequest]].[[view]] to null.
  controller->byob_request_->view_.Reset();
  // 4. Set controller.[[byobRequest]] to null.
  controller->byob_request_ = nullptr;
}

// static
void ReadableByteStreamController::SetUp(
    v8::Isolate* isolate,
    ReadableStream* stream,
    ReadableByteStreamController* controller,
    StreamStartAlgorithm* start_algorithm,
    StreamAlgorithm* pull_algorithm,
    StreamAlgorithm* cancel_algorithm,
    double high_water_mark,
    size_t auto_allocate_chunk_size,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#set-up-readable-byte-stream-controller
  // 1. Assert: stream.[[controller]] is undefined.
  CHECK(!stream->readable_stream_controller_);
  // 2. If autoAllocateChunkSize is not undefined,
  //   a. Assert: ! IsInteger(autoAllocateChunkSize) is true.
  //   b. Assert: autoAllocateChunkSize is positive.
  //   Due to autoAllocateChunkSize having the [EnforceRange] attribute, it
  //   can never be negative, and 0 stands for undefined here.
  // 3. Set controller.[[stream]] to stream.
  controller->controlled_readable_stream_ = stream;
  // 4. Set controller.[[pullAgain]] and controller.[[pulling]] to false.
  CHECK(!controller->pull_again_);
  CHECK(!controller->pulling_);
  // 5. Set controller.[[byobRequest]] to null.
  CHECK(!controller->byob_request_);
  // 6. Perform ! ResetQueue(controller).
  ResetQueue(controller);
  // 7. Set controller.[[closeRequested]] and controller.[[started]] to false.
  CHECK(!controller->close_requested_);
  CHECK(!controller->started_);
  // 8. Set controller.[[strategyHWM]] to highWaterMark.
  controller->strategy_high_water_mark_ = high_water_mark;
  // 9. Set controller.[[pullAlgorithm]] to pullAlgorithm.
  controller->pull_algorithm_ = pull_algorithm;
  // 10. Set controller.[[cancelAlgorithm]] to cancelAlgorithm.
  controller->cancel_algorithm_ = cancel_algorithm;
  // 11. Set controller.[[autoAllocateChunkSize]] to autoAllocateChunkSize.
  controller->auto_allocate_chunk_size_ = auto_allocate_chunk_size;
  // 12. Set controller.[[pendingPullIntos]] to a new empty list.
  CHECK(controller->pending_pull_intos_.empty());
  // 13. Set stream.[[controller]] to controller.
  stream->readable_stream_controller_ = controller;
  // 14. Let startResult be the result of performing startAlgorithm.
  // 15. Let startPromise be a promise resolved with startResult.
  // The conversion of startResult to a promise happens inside start_algorithm
  // in this implementation.
  const bindings::TryRethrowScope rethrow_scope(isolate, exception_state);
  const v8::Local<v8::Promise> start_promise = start_algorithm->Run(isolate);
  if (start_promise.IsEmpty()) {
    CHECK(rethrow_scope.HasCaught());
    return;
  }

  class ResolveFunction final : public bindings::ScriptFunction {
   public:
    explicit ResolveFunction(ReadableByteStreamController* controller)
        : controller_(controller) {}

    v8::Local<v8::Value> Call(v8::Isolate* isolate,
                              v8::Local<v8::Value>) override {
      // 16. Upon fulfillment of startPromise,
      //   a. Set controller.[[started]] to true.
      controller_->started_ = true;
      //   b. Assert: controller.[[pulling]] is false.
      //   c. Assert: controller.[[pullAgain]] is false.
      //   d. Perform !
      //   ReadableByteStreamControllerCallPullIfNeeded(controller).
      CallPullIfNeeded(isolate, controller_);
      return v8::Undefined(isolate);
    }

    void Trace(cppgc::Visitor* visitor) const override {
      visitor->Trace(controller_);
      bindings::ScriptFunction::Trace(visitor);
    }

   private:
    const cppgc::Member<ReadableByteStreamController> controller_;
  };

  class RejectFunction final : public bindings::ScriptFunction {
   public:
    explicit RejectFunction(ReadableByteStreamController* controller)
        : controller_(controller) {}

    v8::Local<v8::Value> Call(v8::Isolate* isolate,
                              v8::Local<v8::Value> r) override {
      // 17. Upon rejection of startPromise with reason r,
      //   a. Perform ! ReadableByteStreamControllerError(controller, r).
      Error(isolate, controller_, r);
      return v8::Undefined(isolate);
    }

    void Trace(cppgc::Visitor* visitor) const override {
      visitor->Trace(controller_);
      bindings::ScriptFunction::Trace(visitor);
    }

   private:
    const cppgc::Member<ReadableByteStreamController> controller_;
  };

  bindings::PromiseThen(
      isolate, start_promise,
      bindings::MakeGarbageCollected<ResolveFunction>(controller),
      bindings::MakeGarbageCollected<RejectFunction>(controller));
}

// static
void ReadableByteStreamController::SetUpFromUnderlyingSource(
    v8::Isolate* isolate,
    ReadableStream* stream,
    v8::Local<v8::Object> underlying_source,
    v8::Local<v8::Value> start_method,
    v8::Local<v8::Value> pull_method,
    v8::Local<v8::Value> cancel_method,
    std::optional<uint64_t> auto_allocate_chunk_size_value,
    double high_water_mark,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#set-up-readable-byte-stream-controller-from-underlying-source
  // 1. Let controller be a new ReadableByteStreamController.
  ReadableByteStreamController* controller =
      bindings::MakeGarbageCollected<ReadableByteStreamController>();
  // 2. Let startAlgorithm be an algorithm that returns undefined.
  StreamStartAlgorithm* start_algorithm = CreateTrivialStartAlgorithm();
  // 3. Let pullAlgorithm be an algorithm that returns a promise resolved with
  // undefined.
  StreamAlgorithm* pull_algorithm = CreateTrivialStreamAlgorithm();
  // 4. Let cancelAlgorithm be an algorithm that returns a promise resolved with
  // undefined.
  StreamAlgorithm* cancel_algorithm = CreateTrivialStreamAlgorithm();

  const v8::Local<v8::Object> controller_value = controller->ToV8(isolate);
  // 5. If underlyingSourceDict["start"] exists, then set startAlgorithm to an
  // algorithm which returns the result of invoking
  // underlyingSourceDict["start"] with argument list « controller » and
  // callback this value underlyingSource.
  if (start_method->IsFunction()) {
    start_algorithm = CreateByteStreamStartAlgorithm(
        isolate, underlying_source, start_method, controller_value);
  }
  // 6. If underlyingSourceDict["pull"] exists, then set pullAlgorithm to an
  // algorithm which returns the result of invoking underlyingSourceDict["pull"]
  // with argument list « controller » and callback this value underlyingSource.
  if (pull_method->IsFunction()) {
    pull_algorithm = CreateAlgorithmFromResolvedMethod(
        isolate, underlying_source, pull_method, controller_value);
  }
  // 7. If underlyingSourceDict["cancel"] exists, then set cancelAlgorithm to an
  // algorithm which takes an argument reason and returns the result of invoking
  // underlyingSourceDict["cancel"] with argument list « reason » and callback
  // this value underlyingSource.
  if (cancel_method->IsFunction()) {
    cancel_algorithm = CreateAlgorithmFromResolvedMethod(
        isolate, underlying_source, cancel_method, controller_value);
  }
  // 8. Let autoAllocateChunkSize be
  // underlyingSourceDict["autoAllocateChunkSize"], if it exists, or undefined
  // otherwise.
  const size_t auto_allocate_chunk_size =
      auto_allocate_chunk_size_value.has_value()
          ? static_cast<size_t>(*auto_allocate_chunk_size_value)
          : 0u;
  // 9. If autoAllocateChunkSize is 0, then throw a TypeError exception.
  if (auto_allocate_chunk_size_value.has_value() &&
      auto_allocate_chunk_size == 0) {
    exception_state.ThrowTypeError("autoAllocateChunkSize cannot be 0");
    return;
  }
  // 10. Perform ? SetUpReadableByteStreamController(stream, controller,
  // startAlgorithm, pullAlgorithm, cancelAlgorithm, highWaterMark,
  // autoAllocateChunkSize).
  SetUp(isolate, stream, controller, start_algorithm, pull_algorithm,
        cancel_algorithm, high_water_mark, auto_allocate_chunk_size,
        exception_state);
}

// static
void ReadableByteStreamController::FillHeadPullIntoDescriptor(
    ReadableByteStreamController* controller,
    size_t size,
    PullIntoDescriptor* pull_into_descriptor) {
  // https://streams.spec.whatwg.org/#readable-byte-stream-controller-fill-head-pull-into-descriptor
  // 1. Assert: either controller.[[pendingPullIntos]] is empty, or
  // controller.[[pendingPullIntos]][0] is pullIntoDescriptor.
  // 2. Assert: controller.[[byobRequest]] is null.
  // 3. Set pullIntoDescriptor’s bytes filled to bytes filled + size.
  pull_into_descriptor->bytes_filled =
      base::CheckAdd(pull_into_descriptor->bytes_filled, size).ValueOrDie();
}

// static
bool ReadableByteStreamController::FillPullIntoDescriptorFromQueue(
    v8::Isolate* isolate,
    ReadableByteStreamController* controller,
    PullIntoDescriptor* pull_into_descriptor,
    bindings::ExceptionState& exception_state) {
  if (pull_into_descriptor->Buffer(isolate)->WasDetached()) {
    exception_state.ThrowTypeError("buffer is detached");
    return false;
  }
  // https://streams.spec.whatwg.org/#readable-byte-stream-controller-fill-pull-into-descriptor-from-queue
  // 1. Let maxBytesToCopy be min(controller.[[queueTotalSize]],
  // pullIntoDescriptor’s byte length − pullIntoDescriptor’s bytes filled).
  // The subtraction will not underflow because bytes length will always be more
  // than or equal to bytes filled.
  const size_t max_bytes_to_copy = std::min(
      static_cast<size_t>(controller->queue_total_size_),
      pull_into_descriptor->byte_length - pull_into_descriptor->bytes_filled);
  // 2. Let maxBytesFilled be pullIntoDescriptor’s bytes filled +
  // maxBytesToCopy.
  // This addition will not overflow because maxBytesToCopy can be at most
  // queue_total_size_. Both bytes_filled and queue_total_size_ refer to
  // actually allocated memory, so together they cannot exceed size_t.
  const size_t max_bytes_filled =
      pull_into_descriptor->bytes_filled + max_bytes_to_copy;
  // 3. Let totalBytesToCopyRemaining be maxBytesToCopy.
  size_t total_bytes_to_copy_remaining = max_bytes_to_copy;
  // 4. Let ready be false;
  bool ready = false;
  // 5. Assert: ! IsDetachedBuffer(pullIntoDescriptor’s buffer) is false.
  CHECK(!pull_into_descriptor->Buffer(isolate)->WasDetached());
  // 6. Assert: pullIntoDescriptor’s bytes filled < pullIntoDescriptor’s
  //    minimum fill.
  CHECK_LT(pull_into_descriptor->bytes_filled,
           pull_into_descriptor->minimum_fill);
  // 7. Let remainderBytes be the remainder after dividing maxBytesFilled by
  //    pullIntoDescriptor’s element size.
  const size_t remainder_bytes =
      max_bytes_filled % pull_into_descriptor->element_size;
  // 8. Let maxAlignedBytes be maxBytesFilled − remainderBytes.
  // This subtraction will not underflow because the modulus operator is
  // guaranteed to return a value less than or equal to the first argument.
  const size_t max_aligned_bytes = max_bytes_filled - remainder_bytes;
  // 9. If maxAlignedBytes >= pullIntoDescriptor’s minimum fill,
  if (max_aligned_bytes >= pull_into_descriptor->minimum_fill) {
    // 1. Set totalBytesToCopyRemaining to maxAlignedBytes −
    // pullIntoDescriptor’s bytes filled.
    total_bytes_to_copy_remaining =
        base::CheckSub(max_aligned_bytes, pull_into_descriptor->bytes_filled)
            .ValueOrDie();
    // 2. Set ready to true.
    ready = true;
  }
  // 10. Let queue be controller.[[queue]].
  HeapDeque<QueueEntry>& queue = controller->queue_;
  // 11. While totalBytesToCopyRemaining > 0,
  while (total_bytes_to_copy_remaining > 0) {
    // 1. Let headOfQueue be queue[0].
    QueueEntry* head_of_queue = queue[0];
    // 2. Let bytesToCopy be min(totalBytesToCopyRemaining,
    // headOfQueue’s byte length).
    const size_t bytes_to_copy =
        std::min(total_bytes_to_copy_remaining, head_of_queue->byte_length);
    // 3. Let destStart be pullIntoDescriptor’s byte offset +
    // pullIntoDescriptor’s bytes filled.
    // This addition will not overflow because byte offset and bytes filled
    // refer to actually allocated memory, so together they cannot exceed
    // size_t.
    const size_t dest_start =
        pull_into_descriptor->byte_offset + pull_into_descriptor->bytes_filled;
    // 4. Let descriptorBuffer = pullIntoDescriptor.buffer
    const v8::Local<v8::ArrayBuffer> descriptor_buffer =
        pull_into_descriptor->Buffer(isolate);
    // 5. Let queueBuffer be headOfQueue’s buffer.
    const v8::Local<v8::ArrayBuffer> queue_buffer =
        head_of_queue->Buffer(isolate);
    // 6: Let queueByteOffset be headOfQueue’s byte offset.
    const size_t queue_byte_offset = head_of_queue->byte_offset;
    // 7.Assert: ! CanCopyDataBlockBytes(descriptorBuffer, destStart,
    // queueBuffer, queueByteOffset, bytesToCopy) is true.
    // Warning: If this assertion were to fail (due to a bug in this
    // specification or its implementation), then the next step may read from or
    // write to potentially invalid memory. The user agent should always check
    // this assertion, and stop in an implementation-defined manner if it fails
    // (e.g. by crashing the process, or by erroring the stream).
    CHECK_LE(dest_start + bytes_to_copy, descriptor_buffer->ByteLength());
    CHECK_LE(queue_byte_offset + bytes_to_copy, queue_buffer->ByteLength());
    // 8. Perform ! CopyDataBlockBytes(pullIntoDescriptor’s
    // buffer.[[ArrayBufferData]], destStart, headOfQueue’s
    // buffer.[[ArrayBufferData]], headOfQueue’s byte offset, bytesToCopy).
    ByteSpan(descriptor_buffer)
        .subspan(dest_start, bytes_to_copy)
        .copy_from(
            ByteSpan(queue_buffer).subspan(queue_byte_offset, bytes_to_copy));
    // 9. If headOfQueue’s byte length is bytesToCopy,
    if (head_of_queue->byte_length == bytes_to_copy) {
      //   1. Remove queue[0].
      queue.pop_front();
    } else {
      // 10. Otherwise,
      //   1. Set headOfQueue’s byte offset to headOfQueue’s byte offset +
      //   bytesToCopy.
      head_of_queue->byte_offset =
          base::CheckAdd(head_of_queue->byte_offset, bytes_to_copy)
              .ValueOrDie();
      //   2. Set headOfQueue’s byte length to headOfQueue’s byte
      //   length − bytesToCopy.
      head_of_queue->byte_length =
          base::CheckSub(head_of_queue->byte_length, bytes_to_copy)
              .ValueOrDie();
    }
    // 11. Set controller.[[queueTotalSize]] to controller.[[queueTotalSize]] −
    // bytesToCopy.
    controller->queue_total_size_ =
        base::CheckSub(controller->queue_total_size_, bytes_to_copy)
            .ValueOrDie();
    // 12. Perform !
    // ReadableByteStreamControllerFillHeadPullIntoDescriptor(controller,
    // bytesToCopy, pullIntoDescriptor).
    FillHeadPullIntoDescriptor(controller, bytes_to_copy, pull_into_descriptor);
    // 13. Set totalBytesToCopyRemaining to totalBytesToCopyRemaining −
    // bytesToCopy.
    // This subtraction will not underflow because bytes_to_copy will always be
    // greater than or equal to total_bytes_to_copy_remaining.
    total_bytes_to_copy_remaining -= bytes_to_copy;
  }
  // 12. If ready is false,
  //   1. Assert: controller.[[queueTotalSize]] is 0.
  //   2. Assert: pullIntoDescriptor’s bytes filled > 0.
  //   3. Assert: pullIntoDescriptor’s bytes filled < pullIntoDescriptor’s
  //      minimum fill.
  // 13. Return ready.
  return ready;
}

// static
void ReadableByteStreamController::FillReadRequestFromQueue(
    v8::Isolate* isolate,
    ReadableByteStreamController* controller,
    ReadRequest* read_request,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontrollerfillreadrequestfromqueue
  // 1. Assert: controller.[[queueTotalSize]] > 0.
  // 2. Let entry be controller.[[queue]][0].
  QueueEntry* entry = controller->queue_[0];
  // 3. Remove entry from controller.[[queue]].
  controller->queue_.pop_front();
  // 4. Set controller.[[queueTotalSize]] to controller.[[queueTotalSize]] −
  // entry’s byte length.
  controller->queue_total_size_ -= entry->byte_length;
  // 5. Perform ! ReadableByteStreamControllerHandleQueueDrain(controller).
  HandleQueueDrain(isolate, controller);
  // 6. Let view be ! Construct(%Uint8Array%, « entry’s buffer, entry’s byte
  // offset, entry’s byte length »).
  const v8::Local<v8::Uint8Array> view = v8::Uint8Array::New(
      entry->Buffer(isolate), entry->byte_offset, entry->byte_length);
  // 7. Perform readRequest’s chunk steps, given view.
  read_request->ChunkSteps(isolate, view, exception_state);
  if (!exception_state.HadException()) {
    controller->controlled_readable_stream_->DidConsumeBytes(
        entry->byte_length);
  }
}

// static
void ReadableByteStreamController::PullInto(
    v8::Isolate* isolate,
    ReadableByteStreamController* controller,
    v8::Local<v8::ArrayBufferView> view,
    uint64_t min,
    ReadIntoRequest* read_into_request,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#readable-byte-stream-controller-pull-into
  // 1. Let stream be controller.[[stream]].
  ReadableStream* const stream = controller->controlled_readable_stream_;
  // 2. Let elementSize be 1.
  size_t element_size = 1;
  // 3. Let ctor be %DataView%.
  PullIntoDescriptor::ViewConstructorType ctor =
      &CreateAsArrayBufferView<v8::DataView>;
  // 4. If view has a [[TypedArrayName]] internal slot (i.e., it is not a
  // DataView),
  if (!view->IsDataView()) {
    //   a. Set elementSize to be the element size specified in the typed array
    //   constructors table for view.[[TypedArrayName]].
    //   b. Set ctor to the constructor specified in the typed array
    //   constructors table for view.[[TypedArrayName]].
    TypedArrayElementSizeAndConstructor(view, &element_size, &ctor);
  }
  // 5. Let minimumFill be min * elementSize.
  const uint64_t minimum_fill = base::CheckMul(min, element_size).ValueOrDie();
  // 6. Assert: minimumFill >= 0 and minimumFill <= view.[[ByteLength]].
  CHECK_LE(minimum_fill, view->ByteLength());
  // 7. Assert: the remainder after dividing minimumFill by elementSize is 0.
  CHECK_EQ(minimum_fill % element_size, 0u);
  // 8. Let byteOffset be view.[[ByteOffset]].
  const size_t byte_offset = view->ByteOffset();
  // 9. Let byteLength be view.[[ByteLength]].
  const size_t byte_length = view->ByteLength();
  // 10. Let bufferResult be TransferArrayBuffer(view.[[ViewedArrayBuffer]]).
  v8::Local<v8::ArrayBuffer> buffer;
  {
    const v8::TryCatch try_catch(isolate);
    bindings::ExceptionState pass_through(isolate);
    buffer = TransferArrayBuffer(isolate, view->Buffer(), pass_through);
    // 11. If bufferResult is an abrupt completion,
    if (try_catch.HasCaught()) {
      //  a. Perform readIntoRequest's error steps, given
      //  bufferResult.[[Value]].
      read_into_request->ErrorSteps(isolate, try_catch.Exception());
      //  b. Return.
      return;
    }
  }
  // 12. Let buffer be bufferResult.[[Value]].

  // 13. Let pullIntoDescriptor be a new pull-into descriptor with buffer
  // buffer, buffer byte length buffer.[[ArrayBufferByteLength]], byte offset
  // byteOffset, byte length byteLength, bytes filled 0, minimum fill
  // minimumFill, element size elementSize, view constructor ctor, and
  // reader type "byob".
  PullIntoDescriptor* pull_into_descriptor =
      bindings::MakeGarbageCollected<PullIntoDescriptor>(
          isolate, buffer, buffer->ByteLength(), byte_offset, byte_length,
          /*bytes_filled=*/0, minimum_fill, element_size, ctor,
          ReaderType::kBYOB);
  // 14. If controller.[[pendingPullIntos]] is not empty,
  if (!controller->pending_pull_intos_.empty()) {
    //   a. Append pullIntoDescriptor to controller.[[pendingPullIntos]].
    controller->pending_pull_intos_.push_back(pull_into_descriptor);
    //   b. Perform ! ReadableStreamAddReadIntoRequest(stream, readIntoRequest).
    ReadableStream::AddReadIntoRequest(isolate, stream, read_into_request);
    //   c. Return.
    return;
  }
  // 15. If stream.[[state]] is "closed",
  if (stream->state_ == ReadableStream::kClosed) {
    //   a. Let emptyView be ! Construct(ctor, « pullIntoDescriptor’s buffer,
    //   pullIntoDescriptor’s byte offset, 0 »).
    const v8::Local<v8::ArrayBufferView> empty_view =
        ctor(pull_into_descriptor->Buffer(isolate),
             pull_into_descriptor->byte_offset, 0);
    //   b. Perform readIntoRequest’s close steps, given emptyView.
    read_into_request->CloseSteps(isolate, empty_view);
    //   c. Return.
    return;
  }
  // 16. If controller.[[queueTotalSize]] > 0,
  if (controller->queue_total_size_ > 0) {
    //   a. If !
    //   ReadableByteStreamControllerFillPullIntoDescriptorFromQueue(controller,
    //   pullIntoDescriptor) is true,
    const v8::TryCatch try_catch(isolate);
    bindings::ExceptionState fill_exception_state(isolate);
    if (FillPullIntoDescriptorFromQueue(
            isolate, controller, pull_into_descriptor, fill_exception_state)) {
      //     i. Let filledView be !
      //     ReadableByteStreamControllerConvertPullIntoDescriptor(
      //     pullIntoDescriptor).
      bindings::ExceptionState convert_exception_state(isolate);
      const v8::Local<v8::ArrayBufferView> filled_view =
          ConvertPullIntoDescriptor(isolate, pull_into_descriptor,
                                    convert_exception_state);
      if (filled_view.IsEmpty()) {
        return;
      }
      //     ii. Perform !
      //     ReadableByteStreamControllerHandleQueueDrain(controller).
      HandleQueueDrain(isolate, controller);
      //     iii. Perform readIntoRequest’s chunk steps, given filledView.
      read_into_request->ChunkSteps(isolate, filled_view, exception_state);
      if (!exception_state.HadException()) {
        stream->DidConsumeBytes(filled_view->ByteLength());
      }
      //     iv. Return.
      return;
    }
    if (try_catch.HasCaught()) {
      // Instead of returning a rejection, which is inconvenient here,
      // call ControllerError(). The only difference this makes is that it
      // happens synchronously, but that should not be observable.
      ReadableByteStreamController::Error(isolate, controller,
                                          try_catch.Exception());
      return;
    }
    //   b. If controller.[[closeRequested]] is true,
    if (controller->close_requested_) {
      //     i. Let e be a TypeError exception.
      const v8::Local<v8::Value> e =
          CreateTypeError(isolate, "close requested");
      //     ii. Perform ! ReadableByteStreamControllerError(controller, e).
      Error(isolate, controller, e);
      //     iii. Perform readIntoRequest’s error steps, given e.
      read_into_request->ErrorSteps(isolate, e);
      //     iv. Return.
      return;
    }
  }
  // 17. Append pullIntoDescriptor to controller.[[pendingPullIntos]].
  controller->pending_pull_intos_.push_back(pull_into_descriptor);
  // 18. Perform ! ReadableStreamAddReadIntoRequest(stream, readIntoRequest).
  ReadableStream::AddReadIntoRequest(isolate, stream, read_into_request);
  // 19. Perform ! ReadableByteStreamControllerCallPullIfNeeded(controller).
  CallPullIfNeeded(isolate, controller);
}

// static
void ReadableByteStreamController::HandleQueueDrain(
    v8::Isolate* isolate,
    ReadableByteStreamController* controller) {
  // https://streams.spec.whatwg.org/#readable-byte-stream-controller-handle-queue-drain
  // 1. Assert: controller.[[stream]].[[state]] is "readable".
  // 2. If controller.[[queueTotalSize]] is 0 and controller.[[closeRequested]]
  // is true,
  if (!controller->queue_total_size_ && controller->close_requested_) {
    //   a. Perform ! ReadableByteStreamControllerClearAlgorithms(controller).
    ClearAlgorithms(controller);
    //   b. Perform ! ReadableStreamClose(controller.[[stream]]).
    ReadableStream::Close(isolate, controller->controlled_readable_stream_);
  } else {
    // 3. Otherwise,
    //   a. Perform ! ReadableByteStreamControllerCallPullIfNeeded(controller).
    CallPullIfNeeded(isolate, controller);
  }
}

// static
void ReadableByteStreamController::ResetQueue(
    ReadableByteStreamController* controller) {
  // https://streams.spec.whatwg.org/#reset-queue
  // 1. Assert: container has [[queue]] and [[queueTotalSize]] internal slots.
  // 2. Set container.[[queue]] to a new empty list.
  controller->queue_.clear();
  // 3. Set container.[[queueTotalSize]] to 0.
  controller->queue_total_size_ = 0;
}

// static
void ReadableByteStreamController::Respond(
    v8::Isolate* isolate,
    ReadableByteStreamController* controller,
    size_t bytes_written,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#readable-byte-stream-controller-respond
  // 1. Assert: controller.[[pendingPullIntos]] is not empty.
  // 2. Let firstDescriptor be controller.[[pendingPullIntos]][0].
  PullIntoDescriptor* first_descriptor = controller->pending_pull_intos_[0];
  // 3. Let state be controller.[[stream]].[[state]].
  const ReadableStream::State state =
      controller->controlled_readable_stream_->state_;
  // 4. If state is "closed",
  if (state == ReadableStream::kClosed) {
    //   a. If bytesWritten is not 0, throw a TypeError exception.
    if (bytes_written != 0) {
      exception_state.ThrowTypeError("bytes written is not 0");
      return;
    }
    // 5. Otherwise,
  } else {
    //   a. Assert: state is "readable".
    //   b. If bytesWritten is 0, throw a TypeError exception.
    if (bytes_written == 0) {
      exception_state.ThrowTypeError("bytes written is 0");
      return;
    }
    //   c. If firstDescriptor's bytes filled + bytesWritten > firstDescriptor's
    //   byte length, throw a RangeError exception.
    if (base::ClampAdd(first_descriptor->bytes_filled, bytes_written) >
        first_descriptor->byte_length) {
      exception_state.ThrowRangeError(
          "available read buffer is too small for specified number of bytes");
      return;
    }
  }
  // 6. Set firstDescriptor's buffer to ! TransferArrayBuffer(firstDescriptor's
  // buffer).
  const v8::Local<v8::ArrayBuffer> transferred_buffer = TransferArrayBuffer(
      isolate, first_descriptor->Buffer(isolate), exception_state);
  if (transferred_buffer.IsEmpty()) {
    return;
  }
  first_descriptor->SetBuffer(isolate, transferred_buffer);
  // 7. Perform ? ReadableByteStreamControllerRespondInternal(controller,
  // bytesWritten).
  RespondInternal(isolate, controller, bytes_written, exception_state);
}

// static
void ReadableByteStreamController::RespondInClosedState(
    v8::Isolate* isolate,
    ReadableByteStreamController* controller,
    PullIntoDescriptor* first_descriptor,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#readable-byte-stream-controller-respond-in-closed-state
  // 1. Assert: the remainder after dividing firstDescriptor’s bytes filled by
  //    firstDescriptor’s element size is 0.
  CHECK_EQ(first_descriptor->bytes_filled % first_descriptor->element_size, 0u);
  // 2. If firstDescriptor’s reader type is "none", perform !
  // ReadableByteStreamControllerShiftPendingPullInto(controller).
  if (first_descriptor->reader_type == ReaderType::kNone) {
    ShiftPendingPullInto(controller);
  }
  // 3. Let stream be controller.[[stream]].
  ReadableStream* const stream = controller->controlled_readable_stream_;
  // 4. If ! ReadableStreamHasBYOBReader(stream) is true,
  if (ReadableStream::HasBYOBReader(stream)) {
    //   a. While ! ReadableStreamGetNumReadIntoRequests(stream) > 0,
    while (ReadableStream::GetNumReadIntoRequests(stream) > 0) {
      //     i. Let pullIntoDescriptor be !
      //     ReadableByteStreamControllerShiftPendingPullInto(controller).
      PullIntoDescriptor* pull_into_descriptor =
          ShiftPendingPullInto(controller);
      //     ii. Perform !
      //     ReadableByteStreamControllerCommitPullIntoDescriptor(stream,
      //     pullIntoDescriptor).
      CommitPullIntoDescriptor(isolate, stream, pull_into_descriptor,
                               exception_state);
    }
  }
}

// static
void ReadableByteStreamController::RespondInReadableState(
    v8::Isolate* isolate,
    ReadableByteStreamController* controller,
    size_t bytes_written,
    PullIntoDescriptor* pull_into_descriptor,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#readable-byte-stream-controller-respond-in-readable-state
  // 1. Assert: pullIntoDescriptor's bytes filled + bytesWritten ≤
  // pullIntoDescriptor's byte length.
  // 2. Perform !
  // ReadableByteStreamControllerFillHeadPullIntoDescriptor(controller,
  // bytesWritten, pullIntoDescriptor).
  FillHeadPullIntoDescriptor(controller, bytes_written, pull_into_descriptor);
  // 3. If pullIntoDescriptor’s reader type is "none",
  if (pull_into_descriptor->reader_type == ReaderType::kNone) {
    //   a. Perform ?
    //   ReadableByteStreamControllerEnqueueDetachedPullIntoToQueue(controller,
    //   pullIntoDescriptor).
    EnqueueDetachedPullIntoToQueue(isolate, controller, pull_into_descriptor);
    //   b. Perform !
    //   ReadableByteStreamControllerProcessPullIntoDescriptorsUsingQueue(
    //   controller).
    ProcessPullIntoDescriptorsUsingQueue(isolate, controller);
    //   c. Return.
    return;
  }
  // 4. If pullIntoDescriptor’s bytes filled < pullIntoDescriptor’s
  //    minimum fill, return.
  if (pull_into_descriptor->bytes_filled < pull_into_descriptor->minimum_fill) {
    // A descriptor for a read() request that is not yet filled up to its
    // minimum length will stay at the head of the queue, so the underlying
    // source can keep filling it.
    return;
  }
  // 5. Perform ! ReadableByteStreamControllerShiftPendingPullInto(controller).
  ShiftPendingPullInto(controller);
  // 6. Let remainderSize be pullIntoDescriptor’s bytes filled mod
  // pullIntoDescriptor’s element size.
  const size_t remainder_size =
      pull_into_descriptor->bytes_filled % pull_into_descriptor->element_size;
  // 7. If remainderSize > 0,
  if (remainder_size > 0) {
    //   a. Let end be pullIntoDescriptor’s byte offset + pullIntoDescriptor’s
    //   bytes filled.
    //   This addition will not overflow because byte offset and bytes filled
    //   refer to actually allocated memory, so together they cannot exceed
    //   size_t.
    const size_t end =
        pull_into_descriptor->byte_offset + pull_into_descriptor->bytes_filled;
    //   b. Perform ?
    //   ReadableByteStreamControllerEnqueueClonedChunkToQueue(controller,
    //   pullIntoDescriptor’s buffer, end − remainderSize, remainderSize).
    EnqueueClonedChunkToQueue(isolate, controller,
                              pull_into_descriptor->Buffer(isolate),
                              end - remainder_size, remainder_size);
  }
  // 8. Set pullIntoDescriptor’s bytes filled to pullIntoDescriptor’s bytes
  // filled − remainderSize.
  pull_into_descriptor->bytes_filled =
      pull_into_descriptor->bytes_filled - remainder_size;
  // 9. Perform !
  // ReadableByteStreamControllerCommitPullIntoDescriptor(controller.[[stream]],
  // pullIntoDescriptor).
  CommitPullIntoDescriptor(isolate, controller->controlled_readable_stream_,
                           pull_into_descriptor, exception_state);
  // 10. Perform !
  // ReadableByteStreamControllerProcessPullIntoDescriptorsUsingQueue(
  // controller).
  ProcessPullIntoDescriptorsUsingQueue(isolate, controller);
}

// static
void ReadableByteStreamController::RespondInternal(
    v8::Isolate* isolate,
    ReadableByteStreamController* controller,
    size_t bytes_written,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#readable-byte-stream-controller-respond-internal
  // 1. Let firstDescriptor be controller.[[pendingPullIntos]][0].
  PullIntoDescriptor* const first_descriptor =
      controller->pending_pull_intos_[0];
  // 2. Assert: ! CanTransferArrayBuffer(firstDescriptor's buffer) is true.
  CHECK(CanTransferArrayBuffer(first_descriptor->Buffer(isolate)));
  // 3. Perform ! ReadableByteStreamControllerInvalidateBYOBRequest(controller).
  InvalidateBYOBRequest(controller);
  // 4. Let state be controller.[[stream]].[[state]].
  const ReadableStream::State state =
      controller->controlled_readable_stream_->state_;
  // 5. If state is "closed",
  if (state == ReadableStream::kClosed) {
    //   a. Assert: bytesWritten is 0
    //   b. Perform !
    //   ReadableByteStreamControllerRespondInClosedState(controller,
    //   firstDescriptor).
    RespondInClosedState(isolate, controller, first_descriptor,
                         exception_state);
  } else {
    // 6. Otherwise,
    //   a. Assert: state is "readable".
    //   b. Assert: bytesWritten > 0.
    //   c. Perform ?
    //   ReadableByteStreamControllerRespondInReadableState(controller,
    //   bytesWritten, firstDescriptor).
    RespondInReadableState(isolate, controller, bytes_written, first_descriptor,
                           exception_state);
  }
  // 7. Perform ! ReadableByteStreamControllerCallPullIfNeeded(controller).
  CallPullIfNeeded(isolate, controller);
}

// static
void ReadableByteStreamController::RespondWithNewView(
    v8::Isolate* isolate,
    ReadableByteStreamController* controller,
    v8::Local<v8::ArrayBufferView> view,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#readable-byte-stream-controller-respond-with-new-view
  // 1. Assert: controller.[[pendingPullIntos]] is not empty.
  // 2. Assert: ! IsDetachedBuffer(view.[[ViewedArrayBuffer]]) is false.
  // 3. Let firstDescriptor be controller.[[pendingPullIntos]][0].
  PullIntoDescriptor* first_descriptor = controller->pending_pull_intos_[0];
  // 4. Let state be controller.[[stream]].[[state]].
  const ReadableStream::State state =
      controller->controlled_readable_stream_->state_;
  // 5. If state is "closed",
  if (state == ReadableStream::kClosed) {
    //   a. If view.[[ByteLength]] is not 0, throw a TypeError exception.
    if (view->ByteLength() != 0) {
      exception_state.ThrowTypeError("view's byte length is not 0");
      return;
    }
    // 6. Otherwise,
  } else {
    //   a. Assert: state is "readable".
    //   b. If view.[[ByteLength]] is 0, throw a TypeError exception.
    if (view->ByteLength() == 0) {
      exception_state.ThrowTypeError("view's byte length is 0");
      return;
    }
  }
  // 7. If firstDescriptor’s byte offset + firstDescriptor’ bytes filled is not
  // view.[[ByteOffset]], throw a RangeError exception.
  // We don't expect this addition to overflow as the bytes are expected to be
  // equal.
  if (first_descriptor->byte_offset + first_descriptor->bytes_filled !=
      view->ByteOffset()) {
    exception_state.ThrowRangeError(
        "supplied view's byte offset doesn't match the expected value");
    return;
  }
  // 8. If firstDescriptor’s buffer byte length is not
  // view.[[ViewedArrayBuffer]].[[ByteLength]], throw a RangeError exception.
  if (first_descriptor->buffer_byte_length != view->Buffer()->ByteLength()) {
    exception_state.ThrowRangeError("buffer byte lengths are not equal");
    return;
  }
  // 9. If firstDescriptor's bytes filled + view.[[ByteLength]] >
  // firstDescriptor's byte length, throw a RangeError exception.
  if (base::ClampAdd(first_descriptor->bytes_filled, view->ByteLength()) >
      first_descriptor->byte_length) {
    exception_state.ThrowRangeError(
        "supplied view is too large for the read buffer");
    return;
  }
  // 10. Let viewByteLength be view.[[ByteLength]].
  const size_t view_byte_length = view->ByteLength();
  // 11. Set firstDescriptor’s buffer to ? TransferArrayBuffer(
  // view.[[ViewedArrayBuffer]]).
  const v8::Local<v8::ArrayBuffer> transferred_buffer =
      TransferArrayBuffer(isolate, view->Buffer(), exception_state);
  if (transferred_buffer.IsEmpty()) {
    return;
  }
  first_descriptor->SetBuffer(isolate, transferred_buffer);
  // 12. Perform ? ReadableByteStreamControllerRespondInternal(controller,
  // viewByteLength).
  RespondInternal(isolate, controller, view_byte_length, exception_state);
}

// static
bool ReadableByteStreamController::CanTransferArrayBuffer(
    v8::Local<v8::ArrayBuffer> buffer) {
  return !buffer->WasDetached();
}

// static
v8::Local<v8::ArrayBuffer> ReadableByteStreamController::TransferArrayBuffer(
    v8::Isolate* isolate,
    v8::Local<v8::ArrayBuffer> buffer,
    bindings::ExceptionState& exception_state) {
  if (!buffer->IsDetachable()) {
    exception_state.ThrowTypeError("Could not transfer ArrayBuffer");
    return {};
  }
  // As DOMArrayBuffer::Transfer() refuses a buffer that is detached already.
  if (buffer->WasDetached()) {
    exception_state.ThrowTypeError("Could not transfer ArrayBuffer.");
    return {};
  }
  std::shared_ptr<v8::BackingStore> contents = buffer->GetBackingStore();
  {
    const bindings::TryRethrowScope rethrow_scope(isolate, exception_state);
    if (buffer->Detach(v8::Local<v8::Value>()).IsNothing()) {
      return {};
    }
  }
  return v8::ArrayBuffer::New(isolate, std::move(contents));
}

void ReadableByteStreamController::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(byob_request_);
  visitor->Trace(cancel_algorithm_);
  visitor->Trace(controlled_readable_stream_);
  pending_pull_intos_.Trace(visitor);
  visitor->Trace(pull_algorithm_);
  queue_.Trace(visitor);
  ReadableStreamController::Trace(visitor);
}

//
// Readable byte stream controller internal methods
//

v8::Local<v8::Promise> ReadableByteStreamController::CancelSteps(
    v8::Isolate* isolate,
    v8::Local<v8::Value> reason) {
  // https://streams.spec.whatwg.org/#rbs-controller-private-cancel
  // 1. Perform ! ReadableByteStreamControllerClearPendingPullIntos(this).
  ClearPendingPullIntos(this);
  // 2. Perform ! ResetQueue(this).
  ResetQueue(this);
  // 3. Let result be the result of performing this.[[cancelAlgorithm]], passing
  // in reason.
  const v8::Local<v8::Promise> result =
      cancel_algorithm_->Run(isolate, base::span_from_ref(reason));
  // 4. Perform ! ReadableByteStreamControllerClearAlgorithms(this).
  ClearAlgorithms(this);
  // 5. Return result.
  return result;
}

void ReadableByteStreamController::PullSteps(
    v8::Isolate* isolate,
    ReadRequest* read_request,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#rbs-controller-private-pull
  // 1. Let stream be this.[[stream]].
  ReadableStream* const stream = controlled_readable_stream_;
  // 2. Assert: ! ReadableStreamHasDefaultReader(stream) is true.
  // 3. If this.[[queueTotalSize]] > 0,
  if (queue_total_size_ > 0) {
    //   a. Assert: ! ReadableStreamGetNumReadRequests(stream) is 0.
    //   b. Perform ! ReadableByteStreamControllerFillReadRequestFromQueue(this,
    //   readRequest).
    FillReadRequestFromQueue(isolate, this, read_request, exception_state);
    //   c. Return.
    return;
  }
  // 4. Let autoAllocateChunkSize be this.[[autoAllocateChunkSize]].
  const size_t auto_allocate_chunk_size = auto_allocate_chunk_size_;
  // 5. If autoAllocateChunkSize is not undefined,
  if (auto_allocate_chunk_size) {
    //   a. Let buffer be Construct(%ArrayBuffer%, « autoAllocateChunkSize »).
    const v8::Local<v8::ArrayBuffer> buffer =
        v8::ArrayBuffer::New(isolate, auto_allocate_chunk_size);
    //   b. If buffer is an abrupt completion,
    //     i. Perform readRequest’s error steps, given buffer.[[Value]].
    //     ii. Return.
    //   This is not needed as v8::ArrayBuffer::New() crashes if it cannot
    //   allocate the memory.

    //   c. Let pullIntoDescriptor be a new pull-into descriptor with:
    //      buffer              buffer.[[Value]],
    //      buffer byte length  autoAllocateChunkSize,
    //      byte offset         0,
    //      byte length         autoAllocateChunkSize,
    //      bytes filled        0,
    //      minimum fill        1,
    //      element size        1,
    //      view constructor    %Uint8Array%,
    //      reader type         "default".
    PullIntoDescriptor* pull_into_descriptor =
        bindings::MakeGarbageCollected<PullIntoDescriptor>(
            isolate, buffer, auto_allocate_chunk_size, /*byte_offset=*/0,
            auto_allocate_chunk_size, /*bytes_filled=*/0, /*minimum_fill=*/1,
            /*element_size=*/1, &CreateAsArrayBufferView<v8::Uint8Array>,
            ReaderType::kDefault);
    //   d. Append pullIntoDescriptor as the last element of
    //   this.[[pendingPullIntos]].
    pending_pull_intos_.push_back(pull_into_descriptor);
  }
  // 6. Perform ! ReadableStreamAddReadRequest(stream, readRequest).
  ReadableStream::AddReadRequest(isolate, stream, read_request);
  // 7. Perform ! ReadableByteStreamControllerCallPullIfNeeded(this).
  CallPullIfNeeded(isolate, this);
}

void ReadableByteStreamController::ReleaseSteps() {
  // https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontroller-releasesteps
  // 1. If this.[[pendingPullIntos]] is not empty,
  if (!pending_pull_intos_.empty()) {
    //   a. Let firstPendingPullInto be this.[[pendingPullIntos]][0].
    PullIntoDescriptor* first_pending_pull_into = pending_pull_intos_[0];
    //   b. Set firstPendingPullInto’s reader type to "none".
    first_pending_pull_into->reader_type = ReaderType::kNone;
    //   c. Set this.[[pendingPullIntos]] to the list « firstPendingPullInto ».
    pending_pull_intos_.clear();
    pending_pull_intos_.push_back(first_pending_pull_into);
  }
}

}  // namespace mitto
