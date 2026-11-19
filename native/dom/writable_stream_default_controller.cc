#include "mitto/dom/writable_stream_default_controller.h"

#include "base/check.h"
#include "base/check_op.h"
#include "base/containers/span.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_function.h"
#include "mitto/bindings/script_promise.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_writable_stream_default_controller.h"
#include "mitto/dom/abort_controller.h"
#include "mitto/dom/miscellaneous_operations.h"
#include "mitto/dom/queue_with_sizes.h"
#include "mitto/dom/stream_algorithms.h"
#include "mitto/dom/writable_stream.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

class WritableStreamDefaultController::ProcessWriteResolveFunction final
    : public bindings::ScriptFunction {
 public:
  ProcessWriteResolveFunction(WritableStream* stream,
                              WritableStreamDefaultController* controller)
      : stream_(stream), controller_(controller) {}

  v8::Local<v8::Value> Call(v8::Isolate* isolate,
                            v8::Local<v8::Value>) override {
    // https://streams.spec.whatwg.org/#writable-stream-default-controller-process-write
    //  4. Upon fulfillment of sinkWritePromise,
    //      a. Perform ! WritableStreamFinishInFlightWrite(stream).
    WritableStream::FinishInFlightWrite(isolate, stream_.Get());

    //      b. Let state be stream.[[state]].
    const WritableStream::State state = stream_->GetState();

    //      c. Assert: state is "writable" or "erroring".
    CHECK(state == WritableStream::kWritable ||
          state == WritableStream::kErroring);

    //      d. Perform ! DequeueValue(controller).
    controller_->queue_->DequeueValue(isolate);

    //      e. If ! WritableStreamCloseQueuedOrInFlight(stream) is false and
    //         state is "writable",
    if (!WritableStream::CloseQueuedOrInFlight(stream_.Get()) &&
        state == WritableStream::kWritable) {
      //          i. Let backpressure be !
      //             WritableStreamDefaultControllerGetBackpressure(
      //             controller).
      const bool backpressure =
          WritableStreamDefaultController::GetBackpressure(controller_.Get());

      //         ii. Perform ! WritableStreamUpdateBackpressure(stream,
      //             backpressure).
      WritableStream::UpdateBackpressure(isolate, stream_.Get(), backpressure);
    }
    //      f. Perform ! WritableStreamDefaultControllerAdvanceQueueIfNeeded(
    //         controller).
    WritableStreamDefaultController::AdvanceQueueIfNeeded(isolate,
                                                          controller_.Get());
    return v8::Undefined(isolate);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(stream_);
    visitor->Trace(controller_);
    bindings::ScriptFunction::Trace(visitor);
  }

 private:
  const cppgc::Member<WritableStream> stream_;
  const cppgc::Member<WritableStreamDefaultController> controller_;
};

class WritableStreamDefaultController::ProcessWriteRejectFunction final
    : public bindings::ScriptFunction {
 public:
  ProcessWriteRejectFunction(WritableStream* stream,
                             WritableStreamDefaultController* controller)
      : stream_(stream), controller_(controller) {}

  v8::Local<v8::Value> Call(v8::Isolate* isolate,
                            v8::Local<v8::Value> reason) override {
    const WritableStream::State state = stream_->GetState();
    //  5. Upon rejection of sinkWritePromise with reason,
    //      a. If stream.[[state]] is "writable", perform !
    //         WritableStreamDefaultControllerClearAlgorithms(controller).
    if (state == WritableStream::kWritable) {
      WritableStreamDefaultController::ClearAlgorithms(controller_.Get());
    }

    //      b. Perform ! WritableStreamFinishInFlightWriteWithError(stream,
    //         reason).
    WritableStream::FinishInFlightWriteWithError(isolate, stream_.Get(),
                                                 reason);
    return v8::Undefined(isolate);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(stream_);
    visitor->Trace(controller_);
    bindings::ScriptFunction::Trace(visitor);
  }

 private:
  const cppgc::Member<WritableStream> stream_;
  const cppgc::Member<WritableStreamDefaultController> controller_;
};

// Only used internally. Not reachable from JavaScript.
WritableStreamDefaultController::WritableStreamDefaultController()
    : queue_(bindings::MakeGarbageCollected<QueueWithSizes>()) {}

WritableStreamDefaultController::~WritableStreamDefaultController() = default;

const bindings::WrapperTypeInfo*
WritableStreamDefaultController::GetWrapperTypeInfo() const {
  return &bindings::V8WritableStreamDefaultController::kWrapperTypeInfo;
}

void WritableStreamDefaultController::Error(
    const std::optional<v8::Local<v8::Value>>& e) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  // https://streams.spec.whatwg.org/#ws-default-controller-error
  //  2. Let state be this.[[controlledWritableStream]].[[state]].
  const WritableStream::State state = controlled_writable_stream_->GetState();

  //  3. If state is not "writable", return.
  if (state != WritableStream::kWritable) {
    return;
  }
  //  4. Perform ! WritableStreamDefaultControllerError(this, e).
  Error(isolate, this, e.value_or(v8::Undefined(isolate)));
}

// Writable Stream Default Controller Internal Methods

v8::Local<v8::Promise> WritableStreamDefaultController::AbortSteps(
    v8::Isolate* isolate,
    v8::Local<v8::Value> reason) {
  // https://streams.spec.whatwg.org/#ws-default-controller-private-abort
  //  1. Let result be the result of performing this.[[abortAlgorithm]], passing
  //     reason.
  const v8::Local<v8::Promise> result =
      abort_algorithm_->Run(isolate, base::span_from_ref(reason));

  //  2. Perform ! WritableStreamDefaultControllerClearAlgorithms(this).
  ClearAlgorithms(this);

  //  3. Return result.
  return result;
}

void WritableStreamDefaultController::ErrorSteps() {
  // https://streams.spec.whatwg.org/#ws-default-controller-private-error
  //  1. Perform ! ResetQueue(this).
  queue_->ResetQueue();
}

// Writable Stream Default Controller Abstract Operations

// static
void WritableStreamDefaultController::SetUp(
    v8::Isolate* isolate,
    WritableStream* stream,
    WritableStreamDefaultController* controller,
    StreamStartAlgorithm* start_algorithm,
    StreamAlgorithm* write_algorithm,
    StreamAlgorithm* close_algorithm,
    StreamAlgorithm* abort_algorithm,
    double high_water_mark,
    StrategySizeAlgorithm* size_algorithm,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#set-up-writable-stream-default-controller
  //  2. Assert: stream.[[writableStreamController]] is undefined.
  CHECK(!stream->Controller());

  //  3. Set controller.[[controlledWritableStream]] to stream.
  controller->controlled_writable_stream_ = stream;

  //  4. Set stream.[[writableStreamController]] to controller.
  stream->SetController(controller);

  // Step not needed because queue is initialised during construction.
  //  5. Perform ! ResetQueue(controller).

  //  6. Set controller.[[abortController]] to a new AbortController.
  controller->abort_controller_ = AbortController::Create();

  //  7. Set controller.[[started]] to false.
  controller->started_ = false;

  //  8. Set controller.[[strategySizeAlgorithm]] to sizeAlgorithm.
  controller->strategy_size_algorithm_ = size_algorithm;

  //  9. Set controller.[[strategyHWM]] to highWaterMark.
  controller->strategy_high_water_mark_ = high_water_mark;

  // 10. Set controller.[[writeAlgorithm]] to writeAlgorithm.
  controller->write_algorithm_ = write_algorithm;

  // 11. Set controller.[[closeAlgorithm]] to closeAlgorithm.
  controller->close_algorithm_ = close_algorithm;

  // 12. Set controller.[[abortAlgorithm]] to abortAlgorithm.
  controller->abort_algorithm_ = abort_algorithm;

  // 13. Let backpressure be !
  //     WritableStreamDefaultControllerGetBackpressure(controller).
  const bool backpressure = GetBackpressure(controller);

  // 14. Perform ! WritableStreamUpdateBackpressure(stream, backpressure).
  WritableStream::UpdateBackpressure(isolate, stream, backpressure);

  // 15. Let startResult be the result of performing startAlgorithm. (This may
  //     throw an exception.)
  // In this implementation, start_algorithm returns a Promise when it doesn't
  // throw.
  // 16. Let startPromise be a promise resolved with startResult.
  const bindings::TryRethrowScope rethrow_scope(isolate, exception_state);
  const v8::Local<v8::Promise> start_promise = start_algorithm->Run(isolate);
  if (start_promise.IsEmpty()) {
    CHECK(rethrow_scope.HasCaught());
    return;
  }

  class ResolvePromiseFunction final : public bindings::ScriptFunction {
   public:
    explicit ResolvePromiseFunction(WritableStream* stream) : stream_(stream) {}

    v8::Local<v8::Value> Call(v8::Isolate* isolate,
                              v8::Local<v8::Value>) override {
      // 17. Upon fulfillment of startPromise
      //      a. Assert: stream.[[state]] is "writable" or "erroring".
      const WritableStream::State state = stream_->GetState();
      CHECK(state == WritableStream::kWritable ||
            state == WritableStream::kErroring);

      //      b. Set controller.[[started]] to true.
      WritableStreamDefaultController* controller = stream_->Controller();
      controller->started_ = true;

      //      c. Perform ! WritableStreamDefaultControllerAdvanceQueueIfNeeded(
      //         controller).
      WritableStreamDefaultController::AdvanceQueueIfNeeded(isolate,
                                                            controller);
      return v8::Undefined(isolate);
    }

    void Trace(cppgc::Visitor* visitor) const override {
      visitor->Trace(stream_);
      bindings::ScriptFunction::Trace(visitor);
    }

   private:
    const cppgc::Member<WritableStream> stream_;
  };

  class RejectPromiseFunction final : public bindings::ScriptFunction {
   public:
    explicit RejectPromiseFunction(WritableStream* stream) : stream_(stream) {}

    v8::Local<v8::Value> Call(v8::Isolate* isolate,
                              v8::Local<v8::Value> r) override {
      // 18. Upon rejection of startPromise with reason r,
      //      a. Assert: stream.[[state]] is "writable" or "erroring".
      const WritableStream::State state = stream_->GetState();
      CHECK(state == WritableStream::kWritable ||
            state == WritableStream::kErroring);

      //      b. Set controller.[[started]] to true.
      WritableStreamDefaultController* controller = stream_->Controller();
      controller->started_ = true;

      //      c. Perform ! WritableStreamDealWithRejection(stream, r).
      WritableStream::DealWithRejection(isolate, stream_.Get(), r);
      return v8::Undefined(isolate);
    }

    void Trace(cppgc::Visitor* visitor) const override {
      visitor->Trace(stream_);
      bindings::ScriptFunction::Trace(visitor);
    }

   private:
    const cppgc::Member<WritableStream> stream_;
  };

  bindings::PromiseThen(
      isolate, start_promise,
      bindings::MakeGarbageCollected<ResolvePromiseFunction>(stream),
      bindings::MakeGarbageCollected<RejectPromiseFunction>(stream));

  controller->resolve_function_ =
      bindings::MakeGarbageCollected<ProcessWriteResolveFunction>(
          controller->controlled_writable_stream_.Get(), controller);
  controller->reject_function_ =
      bindings::MakeGarbageCollected<ProcessWriteRejectFunction>(
          controller->controlled_writable_stream_.Get(), controller);
}

// static
void WritableStreamDefaultController::SetUpFromUnderlyingSink(
    v8::Isolate* isolate,
    WritableStream* stream,
    v8::Local<v8::Object> underlying_sink,
    double high_water_mark,
    StrategySizeAlgorithm* size_algorithm,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#set-up-writable-stream-default-controller-from-underlying-sink
  //  1. Assert: underlyingSink is not undefined.
  CHECK(!underlying_sink.IsEmpty());

  //  2. Let controller be ObjectCreate(the original value of
  //     WritableStreamDefaultController's prototype property).
  auto* controller =
      bindings::MakeGarbageCollected<WritableStreamDefaultController>();

  const v8::Local<v8::Value> controller_value = controller->ToV8(isolate);

  //  3. Let startAlgorithm be the following steps:
  //      a. Return ? InvokeOrNoop(underlyingSink, "start", « controller »).
  auto* start_algorithm = CreateStartAlgorithm(
      isolate, underlying_sink, "underlyingSink.start", controller_value);

  //  4. Let writeAlgorithm be ? CreateAlgorithmFromUnderlyingMethod(
  //     underlyingSink, "write", 1, « controller »).
  auto* write_algorithm = CreateAlgorithmFromUnderlyingMethod(
      isolate, underlying_sink, "write", "underlyingSink.write",
      controller_value, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  CHECK(write_algorithm);

  //  5. Let closeAlgorithm be ? CreateAlgorithmFromUnderlyingMethod(
  //     underlyingSink, "close", 0, « »).
  auto* close_algorithm = CreateAlgorithmFromUnderlyingMethod(
      isolate, underlying_sink, "close", "underlyingSink.close",
      v8::MaybeLocal<v8::Value>(), exception_state);
  if (exception_state.HadException()) {
    return;
  }
  CHECK(close_algorithm);

  //  6. Let abortAlgorithm be ? CreateAlgorithmFromUnderlyingMethod(
  //     underlyingSink, "abort", 1, « »).
  auto* abort_algorithm = CreateAlgorithmFromUnderlyingMethod(
      isolate, underlying_sink, "abort", "underlyingSink.abort",
      v8::MaybeLocal<v8::Value>(), exception_state);
  if (exception_state.HadException()) {
    return;
  }
  CHECK(abort_algorithm);

  //  7. Perform ? SetUpWritableStreamDefaultController(stream, controller,
  //     startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm,
  //     highWaterMark, sizeAlgorithm).
  SetUp(isolate, stream, controller, start_algorithm, write_algorithm,
        close_algorithm, abort_algorithm, high_water_mark, size_algorithm,
        exception_state);
}

// static
void WritableStreamDefaultController::Close(
    v8::Isolate* isolate,
    WritableStreamDefaultController* controller) {
  // https://streams.spec.whatwg.org/#writable-stream-default-controller-close
  //  1. Perform ! EnqueueValueWithSize(controller, "close", 0).
  // The |close_queued_| flag represents the presence of the `"close"` marker
  // in the queue.
  controller->close_queued_ = true;

  //  2. Perform ! WritableStreamDefaultControllerAdvanceQueueIfNeeded(
  //     controller).
  AdvanceQueueIfNeeded(isolate, controller);
}

// static
double WritableStreamDefaultController::GetChunkSize(
    v8::Isolate* isolate,
    WritableStreamDefaultController* controller,
    v8::Local<v8::Value> chunk) {
  if (!controller->strategy_size_algorithm_) {
    // No need to error since the stream is already stopped or stopping.
    return 1;
  }

  // https://streams.spec.whatwg.org/#writable-stream-default-controller-get-chunk-size
  //  1. Let returnValue be the result of performing
  //     controller.[[strategySizeAlgorithm]], passing in chunk, and
  //     interpreting the result as an ECMAScript completion value.
  v8::TryCatch try_catch(isolate);
  const std::optional<double> return_value =
      controller->strategy_size_algorithm_->Run(isolate, chunk);

  //  2. If returnValue is an abrupt completion,
  if (!return_value.has_value()) {
    //      a. Perform ! WritableStreamDefaultControllerErrorIfNeeded(
    //         controller, returnValue.[[Value]]).
    const v8::Local<v8::Value> exception = try_catch.Exception();
    try_catch.Reset();
    ErrorIfNeeded(isolate, controller, exception);

    //      b. Return 1.
    return 1;
  }
  //  3. Return returnValue.[[Value]].
  return return_value.value();
}

// static
double WritableStreamDefaultController::GetDesiredSize(
    const WritableStreamDefaultController* controller) {
  // https://streams.spec.whatwg.org/#writable-stream-default-controller-get-desired-size
  //  1. Return controller.[[strategyHWM]] − controller.[[queueTotalSize]].
  return controller->strategy_high_water_mark_ -
         controller->queue_->TotalSize();
}

// static
void WritableStreamDefaultController::Write(
    v8::Isolate* isolate,
    WritableStreamDefaultController* controller,
    v8::Local<v8::Value> chunk,
    double chunk_size,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#writable-stream-default-controller-write
  // The chunk is represented literally in the queue, rather than being embedded
  // in an object, so the following step is not performed:
  //  1. Let writeRecord be Record {[[chunk]]: chunk}.
  {
    //  2. Let enqueueResult be EnqueueValueWithSize(controller, writeRecord,
    //     chunkSize).
    v8::TryCatch try_catch(isolate);
    bindings::ExceptionState pass_through(isolate);
    controller->queue_->EnqueueValueWithSize(isolate, chunk, chunk_size,
                                             pass_through);

    //  3. If enqueueResult is an abrupt completion,
    if (try_catch.HasCaught()) {
      //      a. Perform ! WritableStreamDefaultControllerErrorIfNeeded(
      //         controller, enqueueResult.[[Value]]).
      const v8::Local<v8::Value> exception = try_catch.Exception();
      try_catch.Reset();
      ErrorIfNeeded(isolate, controller, exception);

      //      b. Return.
      return;
    }
  }
  //  4. Let stream be controller.[[controlledWritableStream]].
  WritableStream* stream = controller->controlled_writable_stream_.Get();

  //  5. If ! WritableStreamCloseQueuedOrInFlight(stream) is false and
  //     stream.[[state]] is "writable",
  if (!WritableStream::CloseQueuedOrInFlight(stream) &&
      stream->GetState() == WritableStream::kWritable) {
    //      a. Let backpressure be !
    //         WritableStreamDefaultControllerGetBackpressure(controller).
    const bool backpressure = GetBackpressure(controller);

    //      b. Perform ! WritableStreamUpdateBackpressure(stream, backpressure).
    WritableStream::UpdateBackpressure(isolate, stream, backpressure);
  }

  //  6. Perform ! WritableStreamDefaultControllerAdvanceQueueIfNeeded(
  //     controller).
  AdvanceQueueIfNeeded(isolate, controller);
}

// static
void WritableStreamDefaultController::ErrorIfNeeded(
    v8::Isolate* isolate,
    WritableStreamDefaultController* controller,
    v8::Local<v8::Value> error) {
  // https://streams.spec.whatwg.org/#writable-stream-default-controller-error-if-needed
  //  1. If controller.[[controlledWritableStream]].[[state]] is "writable",
  //     perform ! WritableStreamDefaultControllerError(controller, error).
  const WritableStream::State state =
      controller->controlled_writable_stream_->GetState();
  if (state == WritableStream::kWritable) {
    Error(isolate, controller, error);
  }
}

void WritableStreamDefaultController::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(abort_algorithm_);
  visitor->Trace(close_algorithm_);
  visitor->Trace(controlled_writable_stream_);
  visitor->Trace(queue_);
  visitor->Trace(abort_controller_);
  visitor->Trace(strategy_size_algorithm_);
  visitor->Trace(write_algorithm_);
  visitor->Trace(resolve_function_);
  visitor->Trace(reject_function_);
  bindings::ScriptWrappable::Trace(visitor);
}

// static
void WritableStreamDefaultController::ClearAlgorithms(
    WritableStreamDefaultController* controller) {
  // https://streams.spec.whatwg.org/#writable-stream-default-controller-clear-algorithms
  //  1. Set controller.[[writeAlgorithm]] to undefined.
  controller->write_algorithm_ = nullptr;

  //  2. Set controller.[[closeAlgorithm]] to undefined.
  controller->close_algorithm_ = nullptr;

  //  3. Set controller.[[abortAlgorithm]] to undefined.
  controller->abort_algorithm_ = nullptr;

  //  4. Set controller.[[strategySizeAlgorithm]] to undefined.
  controller->strategy_size_algorithm_ = nullptr;
}

// static
void WritableStreamDefaultController::AdvanceQueueIfNeeded(
    v8::Isolate* isolate,
    WritableStreamDefaultController* controller) {
  // https://streams.spec.whatwg.org/#writable-stream-default-controller-advance-queue-if-needed
  //  1. Let stream be controller.[[controlledWritableStream]].
  WritableStream* stream = controller->controlled_writable_stream_.Get();

  //  2. If controller.[[started]] is false, return
  if (!controller->started_) {
    return;
  }

  //  3. If stream.[[inFlightWriteRequest]] is not undefined, return.
  if (stream->HasInFlightWriteRequest()) {
    return;
  }

  //  4. Let state be stream.[[state]].
  const WritableStream::State state = stream->GetState();

  //  5. If state is "closed" or "errored", return.
  if (state == WritableStream::kClosed || state == WritableStream::kErrored) {
    return;
  }

  //  6. If state is "erroring",
  if (state == WritableStream::kErroring) {
    //      a. Perform ! WritableStreamFinishErroring(stream).
    WritableStream::FinishErroring(isolate, stream);

    //      b. Return.
    return;
  }

  //  7. If controller.[[queue]] is empty, return.
  if (controller->queue_->IsEmpty()) {
    // Empty queue + |close_queued_| true implies `"close"` marker in queue.
    //  9. If writeRecord is "close", perform !
    //     WritableStreamDefaultControllerProcessClose(controller).
    if (controller->close_queued_) {
      ProcessClose(isolate, controller);
    }
    return;
  }

  //  8. Let writeRecord be ! PeekQueueValue(controller).
  const v8::Local<v8::Value> chunk =
      controller->queue_->PeekQueueValue(isolate);

  // 10. Otherwise, perform ! WritableStreamDefaultControllerProcessWrite(
  //     controller, writeRecord.[[chunk]]).
  // ("Otherwise" here means if the chunk is not a `"close"` marker).
  WritableStreamDefaultController::ProcessWrite(isolate, controller, chunk);
}

// static
void WritableStreamDefaultController::ProcessClose(
    v8::Isolate* isolate,
    WritableStreamDefaultController* controller) {
  // https://streams.spec.whatwg.org/#writable-stream-default-controller-process-close
  //  1. Let stream be controller.[[controlledWritableStream]].
  WritableStream* stream = controller->controlled_writable_stream_.Get();

  //  2. Perform ! WritableStreamMarkCloseRequestInFlight(stream).
  WritableStream::MarkCloseRequestInFlight(stream);

  //  3. Perform ! DequeueValue(controller).
  // Here we "dequeue" the `"close"` marker, which is implied by the
  // |close_queued_| flag, by unsetting the flag.
  //  4. Assert: controller.[[queue]] is empty.
  CHECK(controller->queue_->IsEmpty());
  CHECK(controller->close_queued_);
  controller->close_queued_ = false;

  //  5. Let sinkClosePromise be the result of performing
  //     controller.[[closeAlgorithm]].
  const v8::Local<v8::Promise> sink_close_promise =
      controller->close_algorithm_->Run(isolate, {});

  //  6. Perform ! WritableStreamDefaultControllerClearAlgorithms(controller).
  ClearAlgorithms(controller);

  class ResolveFunction final : public bindings::ScriptFunction {
   public:
    explicit ResolveFunction(WritableStream* stream) : stream_(stream) {}

    v8::Local<v8::Value> Call(v8::Isolate* isolate,
                              v8::Local<v8::Value>) override {
      //  7. Upon fulfillment of sinkClosePromise,
      //      a. Perform ! WritableStreamFinishInFlightClose(stream).
      WritableStream::FinishInFlightClose(isolate, stream_.Get());
      return v8::Undefined(isolate);
    }

    void Trace(cppgc::Visitor* visitor) const override {
      visitor->Trace(stream_);
      bindings::ScriptFunction::Trace(visitor);
    }

   private:
    const cppgc::Member<WritableStream> stream_;
  };

  class RejectFunction final : public bindings::ScriptFunction {
   public:
    explicit RejectFunction(WritableStream* stream) : stream_(stream) {}

    v8::Local<v8::Value> Call(v8::Isolate* isolate,
                              v8::Local<v8::Value> reason) override {
      //  8. Upon rejection of sinkClosePromise with reason reason,
      //      a. Perform ! WritableStreamFinishInFlightCloseWithError(stream,
      //         reason).
      WritableStream::FinishInFlightCloseWithError(isolate, stream_.Get(),
                                                   reason);
      return v8::Undefined(isolate);
    }

    void Trace(cppgc::Visitor* visitor) const override {
      visitor->Trace(stream_);
      bindings::ScriptFunction::Trace(visitor);
    }

   private:
    const cppgc::Member<WritableStream> stream_;
  };

  bindings::PromiseThen(isolate, sink_close_promise,
                        bindings::MakeGarbageCollected<ResolveFunction>(stream),
                        bindings::MakeGarbageCollected<RejectFunction>(stream));
}

// static
void WritableStreamDefaultController::ProcessWrite(
    v8::Isolate* isolate,
    WritableStreamDefaultController* controller,
    v8::Local<v8::Value> chunk) {
  // https://streams.spec.whatwg.org/#writable-stream-default-controller-process-write
  //  1. Let stream be controller.[[controlledWritableStream]].
  WritableStream* stream = controller->controlled_writable_stream_.Get();

  //  2. Perform ! WritableStreamMarkFirstWriteRequestInFlight(stream).
  WritableStream::MarkFirstWriteRequestInFlight(stream);

  //  3. Let sinkWritePromise be the result of performing
  //     controller.[[writeAlgorithm]], passing in chunk.
  const v8::Local<v8::Promise> sink_write_promise =
      controller->write_algorithm_->Run(isolate, base::span_from_ref(chunk));

  bindings::PromiseThen(isolate, sink_write_promise,
                        controller->resolve_function_.Get(),
                        controller->reject_function_.Get());
}

// static
bool WritableStreamDefaultController::GetBackpressure(
    const WritableStreamDefaultController* controller) {
  // https://streams.spec.whatwg.org/#writable-stream-default-controller-get-backpressure
  //  1. Let desiredSize be ! WritableStreamDefaultControllerGetDesiredSize(
  //     controller).
  const double desired_size = GetDesiredSize(controller);

  //  2. Return desiredSize ≤ 0.
  return desired_size <= 0;
}

// static
void WritableStreamDefaultController::Error(
    v8::Isolate* isolate,
    WritableStreamDefaultController* controller,
    v8::Local<v8::Value> error) {
  // https://streams.spec.whatwg.org/#writable-stream-default-controller-error
  //  1. Let stream be controller.[[controlledWritableStream]].
  WritableStream* stream = controller->controlled_writable_stream_.Get();

  //  2. Assert: stream.[[state]] is "writable".
  CHECK_EQ(stream->GetState(), WritableStream::kWritable);

  //  3. Perform ! WritableStreamDefaultControllerClearAlgorithms(controller).
  ClearAlgorithms(controller);

  //  4. Perform ! WritableStreamStartErroring(stream, error).
  WritableStream::StartErroring(isolate, stream, error);
}

AbortSignal* WritableStreamDefaultController::Signal() const {
  return abort_controller_->Signal();
}

void WritableStreamDefaultController::Abort(v8::Isolate* isolate,
                                            v8::Local<v8::Value> reason) {
  abort_controller_->Abort(reason);
}

}  // namespace mitto
