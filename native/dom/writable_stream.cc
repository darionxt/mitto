#include "mitto/dom/writable_stream.h"

#include <stddef.h>

#include <string>

#include "base/check.h"
#include "base/check_op.h"
#include "base/notreached.h"
#include "base/strings/strcat.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_function.h"
#include "mitto/bindings/script_promise.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_writable_stream.h"
#include "mitto/dom/miscellaneous_operations.h"
#include "mitto/dom/pipe_options.h"
#include "mitto/dom/readable_stream.h"
#include "mitto/dom/stream_algorithms.h"
#include "mitto/dom/transferable_streams.h"
#include "mitto/dom/writable_stream_default_controller.h"
#include "mitto/dom/writable_stream_default_writer.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"

// WritableStream as Blink implements it (https://streams.spec.whatwg.org/#ws),
// with the classes, methods and abstract operations in the order of the
// standard, for side-by-side reading.

namespace mitto {

// The Record {[[promise]], [[reason]], [[wasAlreadyErroring]]} of the
// standard.
class WritableStream::PendingAbortRequest final
    : public cppgc::GarbageCollected<PendingAbortRequest> {
 public:
  PendingAbortRequest(v8::Isolate* isolate,
                      bindings::ScriptPromiseResolver* resolver,
                      v8::Local<v8::Value> reason,
                      bool was_already_erroring)
      : resolver_(resolver),
        reason_(isolate, reason),
        was_already_erroring_(was_already_erroring) {}
  PendingAbortRequest(const PendingAbortRequest&) = delete;
  PendingAbortRequest& operator=(const PendingAbortRequest&) = delete;

  bindings::ScriptPromiseResolver* GetResolver() { return resolver_.Get(); }
  v8::Local<v8::Value> Reason(v8::Isolate* isolate) {
    return reason_.Get(isolate);
  }

  bool WasAlreadyErroring() { return was_already_erroring_; }

  void Trace(cppgc::Visitor* visitor) const {
    visitor->Trace(resolver_);
    visitor->Trace(reason_);
  }

 private:
  cppgc::Member<bindings::ScriptPromiseResolver> resolver_;
  v8::TracedReference<v8::Value> reason_;
  const bool was_already_erroring_;
};

// static
WritableStream* WritableStream::Create(
    const std::optional<v8::Local<v8::Value>>& underlying_sink,
    const std::optional<v8::Local<v8::Value>>& strategy,
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  auto* stream = bindings::MakeGarbageCollected<WritableStream>();
  stream->InitInternal(
      isolate, underlying_sink.value_or(v8::Undefined(isolate)),
      strategy.value_or(v8::Undefined(isolate)), exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }
  return stream;
}

WritableStream::WritableStream() = default;
WritableStream::~WritableStream() = default;

const bindings::WrapperTypeInfo* WritableStream::GetWrapperTypeInfo() const {
  return &bindings::V8WritableStream::kWrapperTypeInfo;
}

v8::Local<v8::Promise> WritableStream::Abort(
    const std::optional<v8::Local<v8::Value>>& reason,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#ws-abort
  //  2. If ! IsWritableStreamLocked(this) is true, return a promise rejected
  //     with a TypeError exception.
  if (IsLocked(this)) {
    exception_state.ThrowTypeError("Cannot abort a locked stream");
    return {};
  }

  //  3. Return ! WritableStreamAbort(this, reason).
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  return Abort(isolate, this, reason.value_or(v8::Undefined(isolate)));
}

v8::Local<v8::Promise> WritableStream::Close(
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#ws-close
  // 2. If ! IsWritableStreamLocked(this) is true, return a promise rejected
  // with a TypeError exception.
  if (IsLocked(this)) {
    exception_state.ThrowTypeError("Cannot close a locked stream");
    return {};
  }

  // 3. If ! WritableStreamCloseQueuedOrInFlight(this) is true, return a promise
  // rejected with a TypeError exception.
  if (CloseQueuedOrInFlight(this)) {
    exception_state.ThrowTypeError("Cannot close a closed or closing stream");
    return {};
  }

  return Close(v8::Isolate::GetCurrent(), this);
}

WritableStreamDefaultWriter* WritableStream::GetWriter(
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#ws-get-writer
  //  2. Return ? AcquireWritableStreamDefaultWriter(this).
  auto* writer =
      AcquireDefaultWriter(v8::Isolate::GetCurrent(), this, exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }
  return writer;
}

// General Writable Stream Abstract Operations

// static
bindings::ScriptWrappable* WritableStream::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  auto* const stream =
      static_cast<WritableStream*>(bindings::MakeForKnownGap<WritableStream>(
          info, bindings::V8WritableStream::kWrapperTypeInfo));
  auto* const controller =
      bindings::MakeGarbageCollected<WritableStreamDefaultController>();
  bindings::ExceptionState exception_state(isolate);
  WritableStreamDefaultController::SetUp(
      isolate, stream, controller, CreateTrivialStartAlgorithm(),
      CreateTrivialStreamAlgorithm(), CreateTrivialStreamAlgorithm(),
      CreateTrivialStreamAlgorithm(), /*high_water_mark=*/1,
      CreateDefaultSizeAlgorithm(), exception_state);
  CHECK(!exception_state.HadException());
  return stream;
}

// static
WritableStream* WritableStream::Create(
    v8::Isolate* isolate,
    StreamStartAlgorithm* start_algorithm,
    StreamAlgorithm* write_algorithm,
    StreamAlgorithm* close_algorithm,
    StreamAlgorithm* abort_algorithm,
    double high_water_mark,
    StrategySizeAlgorithm* size_algorithm,
    bindings::ExceptionState& exception_state) {
  CHECK(size_algorithm);

  // https://streams.spec.whatwg.org/#create-writable-stream
  //  3. Assert: ! IsNonNegativeNumber(highWaterMark) is true.
  CHECK_GE(high_water_mark, 0);

  //  4. Let stream be ObjectCreate(the original value of WritableStream's
  //     prototype property).
  //  5. Perform ! InitializeWritableStream(stream).
  auto* stream = bindings::MakeGarbageCollected<WritableStream>();

  //  6. Let controller be ObjectCreate(the original value of
  //     WritableStreamDefaultController's prototype property).
  auto* controller =
      bindings::MakeGarbageCollected<WritableStreamDefaultController>();

  //  7. Perform ? SetUpWritableStreamDefaultController(stream, controller,
  //     startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm,
  //     highWaterMark, sizeAlgorithm).
  WritableStreamDefaultController::SetUp(
      isolate, stream, controller, start_algorithm, write_algorithm,
      close_algorithm, abort_algorithm, high_water_mark, size_algorithm,
      exception_state);

  //  8. Return stream.
  return exception_state.HadException() ? nullptr : stream;
}

// static
void WritableStream::Serialize(v8::Isolate* isolate,
                               MessagePort* port,
                               bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#ws-transfer
  // 1. If ! IsWritableStreamLocked(value) is true, throw a "DataCloneError"
  //    DOMException.
  if (IsLocked(this)) {
    exception_state.ThrowTypeError("Cannot transfer a locked stream");
    return;
  }
  // Done by SerializedScriptValue's transfer of the stream:
  // 2. Let port1 be a new MessagePort in the current Realm.
  // 3. Let port2 be a new MessagePort in the current Realm.
  // 4. Entangle port1 and port2.
  // 5. Let readable be a new ReadableStream in the current Realm.
  // 6. Perform ! SetUpCrossRealmTransformReadable(readable, port1).
  ReadableStream* const readable =
      CreateCrossRealmTransformReadable(isolate, port, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  // 7. Let promise be ! ReadableStreamPipeTo(readable, value, false, false,
  //    false).
  const v8::Local<v8::Promise> promise = ReadableStream::PipeTo(
      isolate, readable, this, bindings::MakeGarbageCollected<PipeOptions>(),
      exception_state);
  // 8. Set promise.[[PromiseIsHandled]] to true.
  if (!promise.IsEmpty()) {
    promise->MarkAsHandled();
  }
}

// static
WritableStream* WritableStream::Deserialize(
    v8::Isolate* isolate,
    MessagePort* port,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#ws-transfer
  // Steps 1 and 2 are done by the deserializer, which gives `port`.
  // We need to execute JavaScript to call "Then" on v8::Promises, inside the
  // deserializer, which forbids it. We will not run author code.
  const v8::Isolate::AllowJavascriptExecutionScope allow_javascript(isolate);
  // 3. Perform ! SetUpCrossRealmTransformWritable(value, port).
  return CreateCrossRealmTransformWritable(isolate, port, exception_state);
}

WritableStreamDefaultWriter* WritableStream::AcquireDefaultWriter(
    v8::Isolate* isolate,
    WritableStream* stream,
    bindings::ExceptionState& exception_state) {
  CHECK(stream);
  // https://streams.spec.whatwg.org/#acquire-writable-stream-default-writer
  //  1. Return ? Construct(WritableStreamDefaultWriter, « stream »).
  auto* writer = bindings::MakeGarbageCollected<WritableStreamDefaultWriter>();
  WritableStreamDefaultWriter::SetUp(isolate, writer, stream, exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }
  return writer;
}

// static
v8::Local<v8::Promise> WritableStream::Abort(v8::Isolate* isolate,
                                             WritableStream* stream,
                                             v8::Local<v8::Value> reason) {
  CHECK(stream);
  // https://streams.spec.whatwg.org/#writable-stream-abort
  //  1. If stream.[[state]] is "closed" or "errored", return a promise resolved
  //     with undefined.
  if (stream->state_ == kClosed || stream->state_ == kErrored) {
    return bindings::ResolvedUndefinedPromise(isolate);
  }

  //  2. Signal abort on stream.[[controller]].[[abortController]] with reason.
  stream->Controller()->Abort(isolate, reason);

  //  3. Let state be stream.[[state]].
  const State state = stream->state_;

  //  4. If state is "closed" or "errored", return a promise resolved with
  //     undefined.
  if (state == kClosed || state == kErrored) {
    return bindings::ResolvedUndefinedPromise(isolate);
  }

  //  5. If stream.[[pendingAbortRequest]] is not undefined, return
  //     stream.[[pendingAbortRequest]]'s promise.
  if (stream->pending_abort_request_) {
    return stream->pending_abort_request_->GetResolver()->Promise(isolate);
  }

  //  6. Assert: state is "writable" or "erroring".
  CHECK(state == kWritable || state == kErroring);

  //  7. Let wasAlreadyErroring be false.
  //  8. If state is "erroring",
  //      a. Set wasAlreadyErroring to true.
  //      b. Set reason to undefined.
  const bool was_already_erroring = state == kErroring;
  if (was_already_erroring) {
    reason = v8::Undefined(isolate);
  }

  //  9. Let promise be a new promise.
  auto* resolver =
      bindings::MakeGarbageCollected<bindings::ScriptPromiseResolver>(isolate);

  // 10. Set stream.[[pendingAbortRequest]] to a new pending abort request
  //     whose promise is promise, reason is reason, and was already erroring is
  //     wasAlreadyErroring.
  stream->pending_abort_request_ =
      bindings::MakeGarbageCollected<PendingAbortRequest>(
          isolate, resolver, reason, was_already_erroring);

  // 11. If wasAlreadyErroring is false, perform ! WritableStreamStartErroring(
  //     stream, reason).
  if (!was_already_erroring) {
    StartErroring(isolate, stream, reason);
  }

  // 12. Return promise.
  return resolver->Promise(isolate);
}

// Writable Stream Abstract Operations Used by Controllers

// static
void WritableStream::AddWriteRequest(
    WritableStream* stream,
    bindings::ScriptPromiseResolver* resolver) {
  // https://streams.spec.whatwg.org/#writable-stream-add-write-request
  //  1. Assert: ! IsWritableStreamLocked(stream) is true.
  CHECK(IsLocked(stream));

  //  2. Assert: stream.[[state]] is "writable".
  CHECK_EQ(stream->state_, kWritable);

  //  3. Let promise be a new promise.
  //  4. Append promise as the last element of stream.[[writeRequests]]
  //  5. Return promise.
  stream->write_requests_.push_back(resolver);
}

// static
v8::Local<v8::Promise> WritableStream::Close(v8::Isolate* isolate,
                                             WritableStream* stream) {
  CHECK(stream);
  // https://streams.spec.whatwg.org/#writable-stream-close
  //  1. Let state be stream.[[state]].
  const State state = stream->GetState();

  //  2. If state is "closed" or "errored", return a promise rejected with a
  //     TypeError exception.
  if (state == kClosed || state == kErrored) {
    return bindings::RejectedPromise(
        isolate,
        CreateCannotActionOnStateStreamException(isolate, "close", state));
  }

  //  3. Assert: state is "writable" or "erroring".
  CHECK(state == kWritable || state == kErroring);

  //  4. Assert: ! WritableStreamCloseQueuedOrInFlight(stream) is false.
  CHECK(!CloseQueuedOrInFlight(stream));

  //  5. Let promise be a new promise.
  auto* resolver =
      bindings::MakeGarbageCollected<bindings::ScriptPromiseResolver>(isolate);

  //  6. Set stream.[[closeRequest]] to promise.
  stream->SetCloseRequest(resolver);

  //  7. Let writer be stream.[[writer]].
  WritableStreamDefaultWriter* writer = stream->writer_.Get();

  //  8. If writer is not undefined, and stream.[[backpressure]] is true, and
  //  state is "writable", resolve writer.[[readyPromise]] with undefined.
  if (writer && stream->HasBackpressure() && state == kWritable) {
    writer->ReadyResolver()->Resolve(isolate);
  }

  //  9. Perform ! WritableStreamDefaultControllerClose(
  //     stream.[[writableStreamController]]).
  WritableStreamDefaultController::Close(isolate, stream->Controller());

  // 10. Return promise.
  return resolver->Promise(isolate);
}

// static
bool WritableStream::CloseQueuedOrInFlight(const WritableStream* stream) {
  // https://streams.spec.whatwg.org/#writable-stream-close-queued-or-in-flight
  //  1. If stream.[[closeRequest]] is undefined and
  //     stream.[[inFlightCloseRequest]] is undefined, return false.
  //  2. Return true.
  return stream->close_request_ || stream->in_flight_close_request_;
}

// static
void WritableStream::DealWithRejection(v8::Isolate* isolate,
                                       WritableStream* stream,
                                       v8::Local<v8::Value> error) {
  CHECK(stream);
  // https://streams.spec.whatwg.org/#writable-stream-deal-with-rejection
  //  1. Let state be stream.[[state]].
  const State state = stream->state_;

  //  2. If state is "writable",
  if (state == kWritable) {
    //      a. Perform ! WritableStreamStartErroring(stream, error).
    StartErroring(isolate, stream, error);

    //      b. Return.
    return;
  }

  //  3. Assert: state is "erroring".
  CHECK_EQ(state, kErroring);

  //  4. Perform ! WritableStreamFinishErroring(stream).
  FinishErroring(isolate, stream);
}

// static
void WritableStream::StartErroring(v8::Isolate* isolate,
                                   WritableStream* stream,
                                   v8::Local<v8::Value> reason) {
  CHECK(stream);
  // https://streams.spec.whatwg.org/#writable-stream-start-erroring
  //  1. Assert: stream.[[storedError]] is undefined.
  CHECK(stream->stored_error_.IsEmpty());

  //  2. Assert: stream.[[state]] is "writable".
  CHECK_EQ(stream->state_, kWritable);

  //  3. Let controller be stream.[[writableStreamController]].
  WritableStreamDefaultController* controller =
      stream->writable_stream_controller_.Get();

  //  4. Assert: controller is not undefined.
  CHECK(controller);

  //  5. Set stream.[[state]] to "erroring".
  stream->state_ = kErroring;

  //  6. Set stream.[[storedError]] to reason.
  stream->stored_error_.Reset(isolate, reason);

  //  7. Let writer be stream.[[writer]].
  WritableStreamDefaultWriter* writer = stream->writer_.Get();

  //  8. If writer is not undefined, perform !
  //     WritableStreamDefaultWriterEnsureReadyPromiseRejected(writer, reason).
  if (writer) {
    WritableStreamDefaultWriter::EnsureReadyPromiseRejected(isolate, writer,
                                                            reason);
  }

  //  9. If ! WritableStreamHasOperationMarkedInFlight(stream) is false and
  //     controller.[[started]] is true, perform !
  //     WritableStreamFinishErroring(stream).
  if (!HasOperationMarkedInFlight(stream) && controller->Started()) {
    FinishErroring(isolate, stream);
  }
}

// static
void WritableStream::FinishErroring(v8::Isolate* isolate,
                                    WritableStream* stream) {
  CHECK(stream);
  // https://streams.spec.whatwg.org/#writable-stream-finish-erroring
  //  1. Assert: stream.[[state]] is "erroring".
  CHECK_EQ(stream->state_, kErroring);

  //  2. Assert: ! WritableStreamHasOperationMarkedInFlight(stream) is false.

  //  3. Set stream.[[state]] to "errored".
  stream->state_ = kErrored;

  //  4. Perform ! stream.[[writableStreamController]].[[ErrorSteps]]().
  stream->writable_stream_controller_->ErrorSteps();

  //  5. Let storedError be stream.[[storedError]].
  const v8::Local<v8::Value> stored_error = stream->stored_error_.Get(isolate);

  //  6. Repeat for each writeRequest that is an element of
  //     stream.[[writeRequests]],
  //      a. Reject writeRequest with storedError.
  RejectPromises(isolate, &stream->write_requests_, stored_error);

  //  7. Set stream.[[writeRequests]] to an empty List.
  stream->write_requests_.clear();

  //  8. If stream.[[pendingAbortRequest]] is undefined,
  if (!stream->pending_abort_request_) {
    //      a. Perform !
    //         WritableStreamRejectCloseAndClosedPromiseIfNeeded(stream).
    RejectCloseAndClosedPromiseIfNeeded(isolate, stream);

    //      b. Return.
    return;
  }

  //  9. Let abortRequest be stream.[[pendingAbortRequest]].
  PendingAbortRequest* abort_request = stream->pending_abort_request_.Get();

  // 10. Set stream.[[pendingAbortRequest]] to undefined.
  stream->pending_abort_request_ = nullptr;

  // 11. If abortRequest.[[wasAlreadyErroring]] is true,
  if (abort_request->WasAlreadyErroring()) {
    //      a. Reject abortRequest.[[promise]] with storedError.
    abort_request->GetResolver()->Reject(isolate, stored_error);

    //      b. Perform !
    //         WritableStreamRejectCloseAndClosedPromiseIfNeeded(stream)
    RejectCloseAndClosedPromiseIfNeeded(isolate, stream);

    //      c. Return.
    return;
  }

  // 12. Let promise be ! stream.[[writableStreamController]].[[AbortSteps]](
  //     abortRequest.[[reason]]).
  const v8::Local<v8::Promise> promise =
      stream->writable_stream_controller_->AbortSteps(
          isolate, abort_request->Reason(isolate));

  class ResolvePromiseFunction final : public bindings::ScriptFunction {
   public:
    ResolvePromiseFunction(WritableStream* stream,
                           bindings::ScriptPromiseResolver* resolver)
        : stream_(stream), resolver_(resolver) {}

    v8::Local<v8::Value> Call(v8::Isolate* isolate,
                              v8::Local<v8::Value>) override {
      // 13. Upon fulfillment of promise,
      //      a. Resolve abortRequest.[[promise]] with undefined.
      resolver_->Resolve(isolate);

      //      b. Perform !
      //         WritableStreamRejectCloseAndClosedPromiseIfNeeded(stream).
      RejectCloseAndClosedPromiseIfNeeded(isolate, stream_.Get());
      return v8::Undefined(isolate);
    }

    void Trace(cppgc::Visitor* visitor) const override {
      visitor->Trace(stream_);
      visitor->Trace(resolver_);
      bindings::ScriptFunction::Trace(visitor);
    }

   private:
    const cppgc::Member<WritableStream> stream_;
    const cppgc::Member<bindings::ScriptPromiseResolver> resolver_;
  };

  class RejectPromiseFunction final : public bindings::ScriptFunction {
   public:
    RejectPromiseFunction(WritableStream* stream,
                          bindings::ScriptPromiseResolver* resolver)
        : stream_(stream), resolver_(resolver) {}

    v8::Local<v8::Value> Call(v8::Isolate* isolate,
                              v8::Local<v8::Value> reason) override {
      // 14. Upon rejection of promise with reason reason,
      //      a. Reject abortRequest.[[promise]] with reason.
      resolver_->Reject(isolate, reason);

      //      b. Perform !
      //         WritableStreamRejectCloseAndClosedPromiseIfNeeded(stream).
      RejectCloseAndClosedPromiseIfNeeded(isolate, stream_.Get());
      return v8::Undefined(isolate);
    }

    void Trace(cppgc::Visitor* visitor) const override {
      visitor->Trace(stream_);
      visitor->Trace(resolver_);
      bindings::ScriptFunction::Trace(visitor);
    }

   private:
    const cppgc::Member<WritableStream> stream_;
    const cppgc::Member<bindings::ScriptPromiseResolver> resolver_;
  };

  bindings::PromiseThen(isolate, promise,
                        bindings::MakeGarbageCollected<ResolvePromiseFunction>(
                            stream, abort_request->GetResolver()),
                        bindings::MakeGarbageCollected<RejectPromiseFunction>(
                            stream, abort_request->GetResolver()));
}

// static
void WritableStream::FinishInFlightWrite(v8::Isolate* isolate,
                                         WritableStream* stream) {
  CHECK(stream);
  // https://streams.spec.whatwg.org/#writable-stream-finish-in-flight-write
  //  1. Assert: stream.[[inFlightWriteRequest]] is not undefined.
  CHECK(stream->in_flight_write_request_);

  //  2. Resolve stream.[[inFlightWriteRequest]] with undefined.
  stream->in_flight_write_request_->Resolve(isolate);

  //  3. Set stream.[[inFlightWriteRequest]] to undefined.
  stream->in_flight_write_request_ = nullptr;
}

// static
void WritableStream::FinishInFlightWriteWithError(v8::Isolate* isolate,
                                                  WritableStream* stream,
                                                  v8::Local<v8::Value> error) {
  CHECK(stream);
  // https://streams.spec.whatwg.org/#writable-stream-finish-in-flight-write-with-error
  //  1. Assert: stream.[[inFlightWriteRequest]] is not undefined.
  CHECK(stream->in_flight_write_request_);

  //  2. Reject stream.[[inFlightWriteRequest]] with error.
  stream->in_flight_write_request_->Reject(isolate, error);

  //  3. Set stream.[[inFlightWriteRequest]] to undefined.
  stream->in_flight_write_request_ = nullptr;

  //  4. Assert: stream.[[state]] is "writable" or "erroring".
  const State state = stream->state_;
  CHECK(state == kWritable || state == kErroring);

  //  5. Perform ! WritableStreamDealWithRejection(stream, error).
  DealWithRejection(isolate, stream, error);
}

// static
void WritableStream::FinishInFlightClose(v8::Isolate* isolate,
                                         WritableStream* stream) {
  CHECK(stream);
  // https://streams.spec.whatwg.org/#writable-stream-finish-in-flight-close
  //  1. Assert: stream.[[inFlightCloseRequest]] is not undefined.
  CHECK(stream->in_flight_close_request_);

  //  2. Resolve stream.[[inFlightCloseRequest]] with undefined.
  stream->in_flight_close_request_->Resolve(isolate);

  //  3. Set stream.[[inFlightCloseRequest]] to undefined.
  stream->in_flight_close_request_ = nullptr;

  //  4. Let state be stream.[[state]].
  const State state = stream->state_;

  //  5. Assert: stream.[[state]] is "writable" or "erroring".
  CHECK(state == kWritable || state == kErroring);

  //  6. If state is "erroring",
  if (state == kErroring) {
    //      a. Set stream.[[storedError]] to undefined.
    stream->stored_error_.Reset();

    //      b. If stream.[[pendingAbortRequest]] is not undefined,
    if (stream->pending_abort_request_) {
      //          i. Resolve stream.[[pendingAbortRequest]].[[promise]] with
      //             undefined.
      stream->pending_abort_request_->GetResolver()->Resolve(isolate);

      //         ii. Set stream.[[pendingAbortRequest]] to undefined.
      stream->pending_abort_request_ = nullptr;
    }
  }

  //  7. Set stream.[[state]] to "closed".
  stream->state_ = kClosed;

  //  8. Let writer be stream.[[writer]].
  WritableStreamDefaultWriter* const writer = stream->writer_.Get();

  //  9. If writer is not undefined, resolve writer.[[closedPromise]] with
  //     undefined.
  if (writer) {
    writer->ClosedResolver()->Resolve(isolate);
  }

  // 10. Assert: stream.[[pendingAbortRequest]] is undefined.
  CHECK(!stream->pending_abort_request_);

  // 11. Assert: stream.[[storedError]] is undefined.
  CHECK(stream->stored_error_.IsEmpty());
}

// static
void WritableStream::FinishInFlightCloseWithError(v8::Isolate* isolate,
                                                  WritableStream* stream,
                                                  v8::Local<v8::Value> error) {
  CHECK(stream);
  // https://streams.spec.whatwg.org/#writable-stream-finish-in-flight-close-with-error
  //  1. Assert: stream.[[inFlightCloseRequest]] is not undefined.
  CHECK(stream->in_flight_close_request_);

  //  2. Reject stream.[[inFlightCloseRequest]] with error.
  stream->in_flight_close_request_->Reject(isolate, error);

  //  3. Set stream.[[inFlightCloseRequest]] to undefined.
  stream->in_flight_close_request_ = nullptr;

  //  4. Assert: stream.[[state]] is "writable" or "erroring".
  const State state = stream->state_;
  CHECK(state == kWritable || state == kErroring);

  //  5. If stream.[[pendingAbortRequest]] is not undefined,
  if (stream->pending_abort_request_) {
    //      a. Reject stream.[[pendingAbortRequest]].[[promise]] with error.
    stream->pending_abort_request_->GetResolver()->Reject(isolate, error);

    //      b. Set stream.[[pendingAbortRequest]] to undefined.
    stream->pending_abort_request_ = nullptr;
  }

  //  6. Perform ! WritableStreamDealWithRejection(stream, error).
  DealWithRejection(isolate, stream, error);
}

// static
void WritableStream::MarkCloseRequestInFlight(WritableStream* stream) {
  // https://streams.spec.whatwg.org/#writable-stream-mark-close-request-in-flight
  //  1. Assert: stream.[[inFlightCloseRequest]] is undefined.
  CHECK(!stream->in_flight_close_request_);

  //  2. Assert: stream.[[closeRequest]] is not undefined.
  CHECK(stream->close_request_);

  //  3. Set stream.[[inFlightCloseRequest]] to stream.[[closeRequest]].
  stream->in_flight_close_request_ = stream->close_request_;

  //  4. Set stream.[[closeRequest]] to undefined.
  stream->close_request_ = nullptr;
}

// static
void WritableStream::MarkFirstWriteRequestInFlight(WritableStream* stream) {
  // https://streams.spec.whatwg.org/#writable-stream-mark-first-write-request-in-flight
  //  1. Assert: stream.[[inFlightWriteRequest]] is undefined.
  CHECK(!stream->in_flight_write_request_);

  //  2. Assert: stream.[[writeRequests]] is not empty.
  CHECK(!stream->write_requests_.empty());

  //  3. Let writeRequest be the first element of stream.[[writeRequests]].
  bindings::ScriptPromiseResolver* write_request =
      stream->write_requests_.front();

  //  4. Remove writeRequest from stream.[[writeRequests]], shifting all other
  //     elements downward (so that the second becomes the first, and so on).
  stream->write_requests_.pop_front();

  //  5. Set stream.[[inFlightWriteRequest]] to writeRequest.
  stream->in_flight_write_request_ = write_request;
}

// static
void WritableStream::UpdateBackpressure(v8::Isolate* isolate,
                                        WritableStream* stream,
                                        bool backpressure) {
  CHECK(stream);
  // https://streams.spec.whatwg.org/#writable-stream-update-backpressure
  //  1. Assert: stream.[[state]] is "writable".
  CHECK_EQ(stream->state_, kWritable);

  //  2. Assert: ! WritableStreamCloseQueuedOrInFlight(stream) is false.
  CHECK(!CloseQueuedOrInFlight(stream));

  //  3. Let writer be stream.[[writer]].
  WritableStreamDefaultWriter* writer = stream->writer_.Get();

  //  4. If writer is not undefined and backpressure is not
  //     stream.[[backpressure]],
  if (writer && backpressure != stream->has_backpressure_) {
    //      a. If backpressure is true, set writer.[[readyPromise]] to a new
    //         promise.
    if (backpressure) {
      writer->ResetReadyPromise(isolate);
    } else {
      //      b. Otherwise,
      //          i. Assert: backpressure is false.
      //         ii. Resolve writer.[[readyPromise]] with undefined.
      writer->ReadyResolver()->Resolve(isolate);
    }
  }

  //  5. Set stream.[[backpressure]] to backpressure.
  stream->has_backpressure_ = backpressure;
}

v8::Local<v8::Value> WritableStream::GetStoredError(
    v8::Isolate* isolate) const {
  return stored_error_.Get(isolate);
}

void WritableStream::SetCloseRequest(
    bindings::ScriptPromiseResolver* close_request) {
  close_request_ = close_request;
}

void WritableStream::SetController(
    WritableStreamDefaultController* controller) {
  writable_stream_controller_ = controller;
}

void WritableStream::SetWriter(WritableStreamDefaultWriter* writer) {
  writer_ = writer;
}

// static
v8::Local<v8::String> WritableStream::CreateCannotActionOnStateStreamMessage(
    v8::Isolate* isolate,
    const char* action,
    const char* state_name) {
  return bindings::ToV8StringFromUtf8(
      isolate,
      base::StrCat({"Cannot ", action, " a ", state_name, " writable stream"}));
}

// static
v8::Local<v8::Value> WritableStream::CreateCannotActionOnStateStreamException(
    v8::Isolate* isolate,
    const char* action,
    State state) {
  const char* state_name = nullptr;
  switch (state) {
    case WritableStream::kClosed:
      state_name = "CLOSED";
      break;

    case WritableStream::kErrored:
      state_name = "ERRORED";
      break;

    default:
      NOTREACHED();
  }
  return v8::Exception::TypeError(
      CreateCannotActionOnStateStreamMessage(isolate, action, state_name));
}

void WritableStream::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(close_request_);
  visitor->Trace(in_flight_write_request_);
  visitor->Trace(in_flight_close_request_);
  visitor->Trace(pending_abort_request_);
  visitor->Trace(stored_error_);
  visitor->Trace(writable_stream_controller_);
  visitor->Trace(writer_);
  write_requests_.Trace(visitor);
  bindings::ScriptWrappable::Trace(visitor);
}

// Not done in the C++ constructor, as calling into JavaScript from it can
// cause GC problems.
void WritableStream::InitInternal(v8::Isolate* isolate,
                                  v8::Local<v8::Value> raw_underlying_sink,
                                  v8::Local<v8::Value> raw_strategy,
                                  bindings::ExceptionState& exception_state) {
  // The first parts of this constructor implementation correspond to the object
  // conversions that are implicit in the definition in the standard:
  // https://streams.spec.whatwg.org/#ws-constructor
  CHECK(!raw_underlying_sink.IsEmpty());
  CHECK(!raw_strategy.IsEmpty());

  const v8::Local<v8::Context> context = isolate->GetCurrentContext();

  v8::Local<v8::Object> underlying_sink;
  ScriptValueToObject(isolate, raw_underlying_sink, &underlying_sink,
                      exception_state);
  if (exception_state.HadException()) {
    return;
  }

  // 2. Let size be ? GetV(strategy, "size").
  // 3. Let highWaterMark be ? GetV(strategy, "highWaterMark").
  StrategyUnpacker strategy_unpacker(isolate, raw_strategy, exception_state);
  if (exception_state.HadException()) {
    return;
  }

  // 4. Let type be ? GetV(underlyingSink, "type").
  v8::Local<v8::Value> type;
  {
    const bindings::TryRethrowScope rethrow_scope(isolate, exception_state);
    if (!underlying_sink
             ->Get(context,
                   v8::String::NewFromUtf8Literal(
                       isolate, "type", v8::NewStringType::kInternalized))
             .ToLocal(&type)) {
      return;
    }
  }

  // 5. If type is not undefined, throw a RangeError exception.
  if (!type->IsUndefined()) {
    exception_state.ThrowRangeError("Invalid type is specified");
    return;
  }

  // 6. Let sizeAlgorithm be ? MakeSizeAlgorithmFromSizeFunction(size).
  auto* size_algorithm =
      strategy_unpacker.MakeSizeAlgorithm(isolate, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  CHECK(size_algorithm);

  // 7. If highWaterMark is undefined, let highWaterMark be 1.
  // 8. Set highWaterMark to ? ValidateAndNormalizeHighWaterMark(highWaterMark).
  const double high_water_mark =
      strategy_unpacker.GetHighWaterMark(isolate, 1, exception_state);
  if (exception_state.HadException()) {
    return;
  }

  // 9. Perform ? SetUpWritableStreamDefaultControllerFromUnderlyingSink(this,
  //    underlyingSink, highWaterMark, sizeAlgorithm).
  WritableStreamDefaultController::SetUpFromUnderlyingSink(
      isolate, this, underlying_sink, high_water_mark, size_algorithm,
      exception_state);
}

// static
bool WritableStream::HasOperationMarkedInFlight(const WritableStream* stream) {
  // https://streams.spec.whatwg.org/#writable-stream-has-operation-marked-in-flight
  //  1. If stream.[[inFlightWriteRequest]] is undefined and
  //     controller.[[inFlightCloseRequest]] is undefined, return false.
  //  2. Return true.
  return stream->in_flight_write_request_ || stream->in_flight_close_request_;
}

// static
void WritableStream::RejectCloseAndClosedPromiseIfNeeded(
    v8::Isolate* isolate,
    WritableStream* stream) {
  CHECK(stream);
  // https://streams.spec.whatwg.org/#writable-stream-reject-close-and-closed-promise-if-needed
  //  1. Assert: stream.[[state]] is "errored".
  CHECK_EQ(stream->state_, kErrored);

  //  2. If stream.[[closeRequest]] is not undefined,
  if (stream->close_request_) {
    //      a. Assert: stream.[[inFlightCloseRequest]] is undefined.
    CHECK(!stream->in_flight_close_request_);

    //      b. Reject stream.[[closeRequest]] with stream.[[storedError]].
    stream->close_request_->Reject(isolate, stream->stored_error_.Get(isolate));

    //      c. Set stream.[[closeRequest]] to undefined.
    stream->close_request_ = nullptr;
  }

  //  3. Let writer be stream.[[writer]].
  WritableStreamDefaultWriter* const writer = stream->writer_.Get();

  //  4. If writer is not undefined,
  if (writer) {
    //      a. Reject writer.[[closedPromise]] with stream.[[storedError]].
    writer->ClosedResolver()->Reject(isolate,
                                     stream->stored_error_.Get(isolate));

    //      b. Set writer.[[closedPromise]].[[PromiseIsHandled]] to true.
    writer->Closed()->MarkAsHandled();
  }
}

// Utility functions (not from the standard).

// static
void WritableStream::RejectPromises(v8::Isolate* isolate,
                                    PromiseQueue* queue,
                                    v8::Local<v8::Value> e) {
  for (size_t i = 0; i < queue->size(); ++i) {
    (*queue)[i]->Reject(isolate, e);
  }
}

}  // namespace mitto
