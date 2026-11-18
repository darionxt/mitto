#include "mitto/dom/writable_stream_default_writer.h"

#include <string>
#include <string_view>

#include "base/check.h"
#include "base/check_op.h"
#include "base/strings/strcat.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_promise.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_writable_stream_default_writer.h"
#include "mitto/dom/writable_stream.h"
#include "mitto/dom/writable_stream_default_controller.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

namespace {

std::string CreateWriterLockReleasedMessage(std::string_view verbed) {
  return base::StrCat(
      {"This writable stream writer has been released and cannot be ", verbed});
}

v8::Local<v8::Value> CreateWriterLockReleasedException(
    v8::Isolate* isolate,
    std::string_view verbed) {
  return v8::Exception::TypeError(bindings::ToV8StringFromUtf8(
      isolate, CreateWriterLockReleasedMessage(verbed)));
}

}  // namespace

// static
WritableStreamDefaultWriter* WritableStreamDefaultWriter::Create(
    WritableStream* stream,
    bindings::ExceptionState& exception_state) {
  auto* writer = bindings::MakeGarbageCollected<WritableStreamDefaultWriter>();
  SetUp(v8::Isolate::GetCurrent(), writer, stream, exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }
  return writer;
}

WritableStreamDefaultWriter::WritableStreamDefaultWriter() = default;
WritableStreamDefaultWriter::~WritableStreamDefaultWriter() = default;

const bindings::WrapperTypeInfo*
WritableStreamDefaultWriter::GetWrapperTypeInfo() const {
  return &bindings::V8WritableStreamDefaultWriter::kWrapperTypeInfo;
}

// static
void WritableStreamDefaultWriter::SetUp(
    v8::Isolate* isolate,
    WritableStreamDefaultWriter* writer,
    WritableStream* stream,
    bindings::ExceptionState& exception_state) {
  //  3. Set this.[[ownerWritableStream]] to stream.
  writer->owner_writable_stream_ = stream;
  writer->closed_resolver_ =
      bindings::MakeGarbageCollected<bindings::ScriptPromiseResolver>(isolate);
  writer->ready_resolver_ =
      bindings::MakeGarbageCollected<bindings::ScriptPromiseResolver>(isolate);

  // https://streams.spec.whatwg.org/#default-writer-constructor
  //  2. If ! IsWritableStreamLocked(stream) is true, throw a TypeError
  //     exception.
  if (WritableStream::IsLocked(stream)) {
    exception_state.ThrowTypeError(
        "Cannot create writer when WritableStream is locked");
    return;
  }
  //  4. Set stream.[[writer]] to this.
  stream->SetWriter(writer);

  //  5. Let state be stream.[[state]].
  const WritableStream::State state = stream->GetState();

  switch (state) {
    //  6. If state is "writable",
    case WritableStream::kWritable: {
      //      a. If ! WritableStreamCloseQueuedOrInFlight(stream) is false and
      //         stream.[[backpressure]] is true, set this.[[readyPromise]] to
      //         a new promise.
      // The step above is done when the resolver is made.

      if (WritableStream::CloseQueuedOrInFlight(stream) ||
          !stream->HasBackpressure()) {
        //      b. Otherwise, set this.[[readyPromise]] to a promise resolved
        //         with undefined.
        writer->ready_resolver_->Resolve(isolate);
      }
      //      c. Set this.[[closedPromise]] to a new promise.
      break;
    }

    //  7. Otherwise, if state is "erroring",
    case WritableStream::kErroring: {
      //      a. Set this.[[readyPromise]] to a promise rejected with
      //         stream.[[storedError]].
      writer->ready_resolver_->Promise(isolate)->MarkAsSilent();
      writer->ready_resolver_->Reject(isolate, stream->GetStoredError(isolate));

      //      b. Set this.[[readyPromise]].[[PromiseIsHandled]] to true.
      writer->ready_resolver_->Promise(isolate)->MarkAsHandled();

      //      c. Set this.[[closedPromise]] to a new promise.
      break;
    }

    //  8. Otherwise, if state is "closed",
    case WritableStream::kClosed: {
      //      a. Set this.[[readyPromise]] to a promise resolved with undefined.
      writer->ready_resolver_->Resolve(isolate);

      //      b. Set this.[[closedPromise]] to a promise resolved with
      //         undefined.
      writer->closed_resolver_->Resolve(isolate);
      break;
    }

    //  9. Otherwise,
    case WritableStream::kErrored: {
      //      a. Assert: state is "errored".
      // Check omitted as it is not meaningful.

      //      b. Let storedError be stream.[[storedError]].
      const v8::Local<v8::Value> stored_error = stream->GetStoredError(isolate);

      //      c. Set this.[[readyPromise]] to a promise rejected with
      //         storedError.
      writer->ready_resolver_->Promise(isolate)->MarkAsSilent();
      writer->ready_resolver_->Reject(isolate, stored_error);

      //      d. Set this.[[readyPromise]].[[PromiseIsHandled]] to true.
      writer->ready_resolver_->Promise(isolate)->MarkAsHandled();

      //      e. Set this.[[closedPromise]] to a promise rejected with
      //         storedError.
      writer->closed_resolver_->Promise(isolate)->MarkAsSilent();
      writer->closed_resolver_->Reject(isolate, stored_error);

      //      f. Set this.[[closedPromise]].[[PromiseIsHandled]] to true.
      writer->closed_resolver_->Promise(isolate)->MarkAsHandled();
      break;
    }
  }
}

v8::Local<v8::Promise> WritableStreamDefaultWriter::Closed() const {
  // https://streams.spec.whatwg.org/#default-writer-closed
  //  2. Return this.[[closedPromise]].
  return closed_resolver_->Promise(v8::Isolate::GetCurrent());
}

v8::Local<v8::Value> WritableStreamDefaultWriter::DesiredSize(
    bindings::ExceptionState& exception_state) const {
  // https://streams.spec.whatwg.org/#default-writer-desired-size
  //  2. If this.[[ownerWritableStream]] is undefined, throw a TypeError
  //     exception.
  if (!owner_writable_stream_) {
    exception_state.ThrowTypeError(
        CreateWriterLockReleasedMessage("used to get the desiredSize"));
    return {};
  }

  //  3. Return ! WritableStreamDefaultWriterGetDesiredSize(this).
  return GetDesiredSize(v8::Isolate::GetCurrent(), this);
}

v8::Local<v8::Promise> WritableStreamDefaultWriter::Ready() const {
  // https://streams.spec.whatwg.org/#default-writer-ready
  //  2. Return this.[[readyPromise]].
  return ready_resolver_->Promise(v8::Isolate::GetCurrent());
}

v8::Local<v8::Promise> WritableStreamDefaultWriter::Abort(
    const std::optional<v8::Local<v8::Value>>& reason,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#default-writer-abort
  //  2. If this.[[ownerWritableStream]] is undefined, return a promise rejected
  //     with a TypeError exception.
  if (!owner_writable_stream_) {
    exception_state.ThrowTypeError(CreateWriterLockReleasedMessage("aborted"));
    return {};
  }

  //  3. Return ! WritableStreamDefaultWriterAbort(this, reason).
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  return Abort(isolate, this, reason.value_or(v8::Undefined(isolate)));
}

v8::Local<v8::Promise> WritableStreamDefaultWriter::Close(
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#default-writer-close
  //  2. Let stream be this.[[ownerWritableStream]].
  WritableStream* stream = owner_writable_stream_.Get();

  //  3. If stream is undefined, return a promise rejected with a TypeError
  //     exception.
  if (!stream) {
    exception_state.ThrowTypeError(CreateWriterLockReleasedMessage("closed"));
    return {};
  }

  //  4. If ! WritableStreamCloseQueuedOrInFlight(stream) is true, return a
  //      promise rejected with a TypeError exception.
  if (WritableStream::CloseQueuedOrInFlight(stream)) {
    exception_state.ThrowTypeError(
        "Cannot close a writable stream that has "
        "already been requested to be closed");
    return {};
  }

  //  5. Return ! WritableStreamDefaultWriterClose(this).
  return Close(v8::Isolate::GetCurrent(), this);
}

void WritableStreamDefaultWriter::ReleaseLock() {
  // https://streams.spec.whatwg.org/#default-writer-release-lock
  //  2. Let stream be this.[[ownerWritableStream]].
  WritableStream* stream = owner_writable_stream_.Get();

  //  3. If stream is undefined, return.
  if (!stream) {
    return;
  }

  //  4. Assert: stream.[[writer]] is not undefined.
  CHECK(stream->Writer());

  //  5. Perform ! WritableStreamDefaultWriterRelease(this).
  Release(v8::Isolate::GetCurrent(), this);
}

v8::Local<v8::Promise> WritableStreamDefaultWriter::Write(
    const std::optional<v8::Local<v8::Value>>& chunk,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#default-writer-write
  //  2. If this.[[ownerWritableStream]] is undefined, return a promise rejected
  //     with a TypeError exception.
  if (!owner_writable_stream_) {
    exception_state.ThrowTypeError(
        CreateWriterLockReleasedMessage("written to"));
    return {};
  }

  //  3. Return ! WritableStreamDefaultWriterWrite(this, chunk).
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  return Write(isolate, this, chunk.value_or(v8::Undefined(isolate)),
               exception_state);
}

// static
void WritableStreamDefaultWriter::EnsureReadyPromiseRejected(
    v8::Isolate* isolate,
    WritableStreamDefaultWriter* writer,
    v8::Local<v8::Value> error) {
  // https://streams.spec.whatwg.org/#writable-stream-default-writer-ensure-ready-promise-rejected
  //  1. If writer.[[readyPromise]].[[PromiseState]] is "pending", reject
  //     writer.[[readyPromise]] with error.
  if (writer->ready_resolver_->Promise(isolate)->State() !=
      v8::Promise::kPending) {
    //  2. Otherwise, set writer.[[readyPromise]] to a promise rejected with
    //     error.
    writer->ready_resolver_ =
        bindings::MakeGarbageCollected<bindings::ScriptPromiseResolver>(
            isolate);
  }
  writer->ready_resolver_->Promise(isolate)->MarkAsSilent();
  writer->ready_resolver_->Reject(isolate, error);

  //  3. Set writer.[[readyPromise]].[[PromiseIsHandled]] to true.
  writer->ready_resolver_->Promise(isolate)->MarkAsHandled();
}

// static
v8::Local<v8::Promise> WritableStreamDefaultWriter::CloseWithErrorPropagation(
    v8::Isolate* isolate,
    WritableStreamDefaultWriter* writer) {
  // https://streams.spec.whatwg.org/#writable-stream-default-writer-close-with-error-propagation
  //  1. Let stream be writer.[[ownerWritableStream]].
  WritableStream* stream = writer->owner_writable_stream_.Get();

  //  2. Assert: stream is not undefined.
  CHECK(stream);

  //  3. Let state be stream.[[state]].
  const WritableStream::State state = stream->GetState();

  //  4. If ! WritableStreamCloseQueuedOrInFlight(stream) is true or state is
  //     "closed", return a promise resolved with undefined.
  if (WritableStream::CloseQueuedOrInFlight(stream) ||
      state == WritableStream::kClosed) {
    return bindings::ResolvedUndefinedPromise(isolate);
  }

  //  5. If state is "errored", return a promise rejected with
  //     stream.[[storedError]].
  if (state == WritableStream::kErrored) {
    return bindings::RejectedPromise(isolate, stream->GetStoredError(isolate));
  }

  //  6. Assert: state is "writable" or "erroring".
  CHECK(state == WritableStream::kWritable ||
        state == WritableStream::kErroring);

  //  7. Return ! WritableStreamDefaultWriterClose(writer).
  return Close(isolate, writer);
}

// static
void WritableStreamDefaultWriter::Release(v8::Isolate* isolate,
                                          WritableStreamDefaultWriter* writer) {
  // https://streams.spec.whatwg.org/#writable-stream-default-writer-release
  //  1. Let stream be writer.[[ownerWritableStream]].
  WritableStream* stream = writer->owner_writable_stream_.Get();

  //  2. Assert: stream is not undefined.
  CHECK(stream);

  //  3. Assert: stream.[[writer]] is writer.
  CHECK_EQ(stream->Writer(), writer);

  //  4. Let releasedError be a new TypeError.
  const v8::Local<v8::Value> released_error =
      v8::Exception::TypeError(v8::String::NewFromUtf8Literal(
          isolate,
          "This writable stream writer has been released and cannot be used to "
          "monitor the stream's state"));

  //  5. Perform ! WritableStreamDefaultWriterEnsureReadyPromiseRejected(writer,
  //     releasedError).
  EnsureReadyPromiseRejected(isolate, writer, released_error);

  //  6. Perform !
  //     WritableStreamDefaultWriterEnsureClosedPromiseRejected(writer,
  //     releasedError).
  EnsureClosedPromiseRejected(isolate, writer, released_error);

  //  7. Set stream.[[writer]] to undefined.
  stream->SetWriter(nullptr);

  //  8. Set writer.[[ownerWritableStream]] to undefined.
  writer->owner_writable_stream_ = nullptr;
}

// static
v8::Local<v8::Promise> WritableStreamDefaultWriter::Write(
    v8::Isolate* isolate,
    WritableStreamDefaultWriter* writer,
    v8::Local<v8::Value> chunk,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#writable-stream-default-writer-write
  //  1. Let stream be writer.[[ownerWritableStream]].
  WritableStream* stream = writer->owner_writable_stream_.Get();

  //  2. Assert: stream is not undefined.
  CHECK(stream);

  //  3. Let controller be stream.[[writableStreamController]].
  WritableStreamDefaultController* controller = stream->Controller();

  //  4. Let chunkSize be !
  //     WritableStreamDefaultControllerGetChunkSize(controller, chunk).
  const double chunk_size =
      WritableStreamDefaultController::GetChunkSize(isolate, controller, chunk);

  auto* resolver =
      bindings::MakeGarbageCollected<bindings::ScriptPromiseResolver>(isolate);

  //  5. If stream is not equal to writer.[[ownerWritableStream]], return a
  //     promise rejected with a TypeError exception.
  if (stream != writer->owner_writable_stream_.Get()) {
    resolver->Reject(isolate,
                     CreateWriterLockReleasedException(isolate, "written to"));
    return resolver->Promise(isolate);
  }

  //  6. Let state be stream.[[state]].
  const WritableStream::State state = stream->GetState();

  //  7. If state is "errored", return a promise rejected with
  //     stream.[[storedError]].
  if (state == WritableStream::kErrored) {
    resolver->Reject(isolate, stream->GetStoredError(isolate));
    return resolver->Promise(isolate);
  }

  //  8. If ! WritableStreamCloseQueuedOrInFlight(stream) is true or state is
  //     "closed", return a promise rejected with a TypeError exception
  //     indicating that the stream is closing or closed.
  if (WritableStream::CloseQueuedOrInFlight(stream)) {
    resolver->Reject(isolate,
                     v8::Exception::TypeError(
                         WritableStream::CreateCannotActionOnStateStreamMessage(
                             isolate, "write to", "closing")));
    return resolver->Promise(isolate);
  }
  if (state == WritableStream::kClosed) {
    resolver->Reject(isolate,
                     WritableStream::CreateCannotActionOnStateStreamException(
                         isolate, "write to", WritableStream::kClosed));
    return resolver->Promise(isolate);
  }

  //  9. If state is "erroring", return a promise rejected with
  //     stream.[[storedError]].
  if (state == WritableStream::kErroring) {
    resolver->Reject(isolate, stream->GetStoredError(isolate));
    return resolver->Promise(isolate);
  }

  // 10. Assert: state is "writable".
  CHECK_EQ(state, WritableStream::kWritable);

  // 11. Let promise be ! WritableStreamAddWriteRequest(stream).
  WritableStream::AddWriteRequest(stream, resolver);

  // 12. Perform ! WritableStreamDefaultControllerWrite(controller, chunk,
  //     chunkSize).
  WritableStreamDefaultController::Write(isolate, controller, chunk, chunk_size,
                                         exception_state);

  // 13. Return promise.
  return resolver->Promise(isolate);
}

std::optional<double> WritableStreamDefaultWriter::GetDesiredSizeInternal()
    const {
  // https://streams.spec.whatwg.org/#writable-stream-default-writer-get-desired-size
  //  1. Let stream be writer.[[ownerWritableStream]].
  const WritableStream* stream = owner_writable_stream_.Get();

  //  2. Let state be stream.[[state]].
  const WritableStream::State state = stream->GetState();

  switch (state) {
    //  3. If state is "errored" or "erroring", return null.
    case WritableStream::kErrored:
    case WritableStream::kErroring:
      return std::nullopt;

      //  4. If state is "closed", return 0.
    case WritableStream::kClosed:
      return 0.0;

    default:
      //  5. Return ! WritableStreamDefaultControllerGetDesiredSize(
      //     stream.[[writableStreamController]]).
      return WritableStreamDefaultController::GetDesiredSize(
          stream->Controller());
  }
}

void WritableStreamDefaultWriter::ResetReadyPromise(v8::Isolate* isolate) {
  ready_resolver_ =
      bindings::MakeGarbageCollected<bindings::ScriptPromiseResolver>(isolate);
}

void WritableStreamDefaultWriter::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(owner_writable_stream_);
  visitor->Trace(closed_resolver_);
  visitor->Trace(ready_resolver_);
  bindings::ScriptWrappable::Trace(visitor);
}

// Writable Stream Writer Abstract Operations

// static
v8::Local<v8::Promise> WritableStreamDefaultWriter::Abort(
    v8::Isolate* isolate,
    WritableStreamDefaultWriter* writer,
    v8::Local<v8::Value> reason) {
  // https://streams.spec.whatwg.org/#writable-stream-default-writer-abort
  //  1. Let stream be writer.[[ownerWritableStream]].
  WritableStream* stream = writer->owner_writable_stream_.Get();

  //  2. Assert: stream is not undefined.
  CHECK(stream);

  //  3. Return ! WritableStreamAbort(stream, reason).
  return WritableStream::Abort(isolate, stream, reason);
}

// static
v8::Local<v8::Promise> WritableStreamDefaultWriter::Close(
    v8::Isolate* isolate,
    WritableStreamDefaultWriter* writer) {
  // https://streams.spec.whatwg.org/#writable-stream-default-writer-close
  //  1. Let stream be writer.[[ownerWritableStream]].
  WritableStream* stream = writer->owner_writable_stream_.Get();

  //  2. Assert: stream is not undefined.
  CHECK(stream);

  //  3. Return ! WritableStreamClose(stream).
  return WritableStream::Close(isolate, stream);
}

// static
void WritableStreamDefaultWriter::EnsureClosedPromiseRejected(
    v8::Isolate* isolate,
    WritableStreamDefaultWriter* writer,
    v8::Local<v8::Value> error) {
  // https://streams.spec.whatwg.org/#writable-stream-default-writer-ensure-closed-promise-rejected
  //  1. If writer.[[closedPromise]].[[PromiseState]] is "pending", reject
  //     writer.[[closedPromise]] with error.
  if (writer->closed_resolver_->Promise(isolate)->State() !=
      v8::Promise::kPending) {
    //  2. Otherwise, set writer.[[closedPromise]] to a promise rejected with
    //     error.
    writer->closed_resolver_ =
        bindings::MakeGarbageCollected<bindings::ScriptPromiseResolver>(
            isolate);
  }
  writer->closed_resolver_->Promise(isolate)->MarkAsSilent();
  writer->closed_resolver_->Reject(isolate, error);

  //  3. Set writer.[[closedPromise]].[[PromiseIsHandled]] to true.
  writer->closed_resolver_->Promise(isolate)->MarkAsHandled();
}

// static
v8::Local<v8::Value> WritableStreamDefaultWriter::GetDesiredSize(
    v8::Isolate* isolate,
    const WritableStreamDefaultWriter* writer) {
  // https://streams.spec.whatwg.org/#writable-stream-default-writer-get-desired-size
  //  1. Let stream be writer.[[ownerWritableStream]].
  //  2. Let state be stream.[[state]].
  //  3. If state is "errored" or "erroring", return null.
  const std::optional<double> desired_size = writer->GetDesiredSizeInternal();
  if (!desired_size.has_value()) {
    return v8::Null(isolate);
  }

  //  4. If state is "closed", return 0.
  //  5. Return ! WritableStreamDefaultControllerGetDesiredSize(
  //     stream.[[writableStreamController]]).
  return v8::Number::New(isolate, desired_size.value());
}

}  // namespace mitto
