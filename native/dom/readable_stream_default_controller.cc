#include "mitto/dom/readable_stream_default_controller.h"

#include <optional>

#include "base/check.h"
#include "base/containers/span.h"
#include "base/notreached.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_function.h"
#include "mitto/bindings/script_promise.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_readable_stream_default_controller.h"
#include "mitto/dom/miscellaneous_operations.h"
#include "mitto/dom/queue_with_sizes.h"
#include "mitto/dom/read_request.h"
#include "mitto/dom/readable_stream.h"
#include "mitto/dom/stream_algorithms.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

class ReadableStreamDefaultController::CallPullIfNeededResolveFunction final
    : public bindings::ScriptFunction {
 public:
  explicit CallPullIfNeededResolveFunction(
      ReadableStreamDefaultController* controller)
      : controller_(controller) {}

  v8::Local<v8::Value> Call(v8::Isolate* isolate,
                            v8::Local<v8::Value>) override {
    // ReadableStreamDefaultControllerCallPullIfNeeded() step 7. Upon
    // fulfillment of pullPromise: set controller.[[pulling]] to false, and if
    // controller.[[pullAgain]] is true, set it to false and call pull again.
    controller_->is_pulling_ = false;
    if (controller_->will_pull_again_) {
      controller_->will_pull_again_ = false;
      CallPullIfNeeded(isolate, controller_);
    }
    return v8::Undefined(isolate);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(controller_);
    bindings::ScriptFunction::Trace(visitor);
  }

 private:
  const cppgc::Member<ReadableStreamDefaultController> controller_;
};

class ReadableStreamDefaultController::CallPullIfNeededRejectFunction final
    : public bindings::ScriptFunction {
 public:
  explicit CallPullIfNeededRejectFunction(
      ReadableStreamDefaultController* controller)
      : controller_(controller) {}

  v8::Local<v8::Value> Call(v8::Isolate* isolate,
                            v8::Local<v8::Value> e) override {
    // Step 8. Upon rejection of pullPromise with reason e, perform !
    // ReadableStreamDefaultControllerError(controller, e).
    Error(isolate, controller_, e);
    return v8::Undefined(isolate);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(controller_);
    bindings::ScriptFunction::Trace(visitor);
  }

 private:
  const cppgc::Member<ReadableStreamDefaultController> controller_;
};

ReadableStreamDefaultController::ReadableStreamDefaultController()
    : queue_(bindings::MakeGarbageCollected<QueueWithSizes>()),
      resolve_function_(
          bindings::MakeGarbageCollected<CallPullIfNeededResolveFunction>(
              this)),
      reject_function_(
          bindings::MakeGarbageCollected<CallPullIfNeededRejectFunction>(
              this)) {}

ReadableStreamDefaultController::~ReadableStreamDefaultController() = default;

bool ReadableStreamDefaultController::IsDefaultController() const {
  return true;
}

bool ReadableStreamDefaultController::IsByteStreamController() const {
  return false;
}

const bindings::WrapperTypeInfo*
ReadableStreamDefaultController::GetWrapperTypeInfo() const {
  return &bindings::V8ReadableStreamDefaultController::kWrapperTypeInfo;
}

void ReadableStreamDefaultController::Close(
    bindings::ExceptionState& exception_state) {
  // Step 1. If ! ReadableStreamDefaultControllerCanCloseOrEnqueue(this) is
  // false, throw a TypeError exception, whose message says why.
  if (!CanCloseOrEnqueue(this)) {
    const char* error_description = nullptr;
    if (is_close_requested_) {
      error_description =
          "Cannot close a readable stream that has already been requested to "
          "be closed";
    } else {
      switch (controlled_readable_stream_->state_) {
        case ReadableStream::kErrored:
          error_description = "Cannot close an errored readable stream";
          break;
        case ReadableStream::kClosed:
          // Blink gives this message for a closed stream too.
          error_description = "Cannot close an errored readable stream";
          break;
        case ReadableStream::kReadable:
          NOTREACHED();
      }
    }
    exception_state.ThrowTypeError(error_description);
    return;
  }
  // Step 2. Perform ! ReadableStreamDefaultControllerClose(this).
  Close(exception_state.GetIsolate(), this);
}

void ReadableStreamDefaultController::Enqueue(
    const std::optional<v8::Local<v8::Value>>& chunk,
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  // Step 1. If ! ReadableStreamDefaultControllerCanCloseOrEnqueue(this) is
  // false, throw a TypeError exception.
  if (!CanCloseOrEnqueue(this)) {
    exception_state.ThrowTypeError(EnqueueExceptionMessage(this));
    return;
  }
  // Step 2. Perform ? ReadableStreamDefaultControllerEnqueue(this, chunk).
  Enqueue(isolate, this, chunk.value_or(v8::Undefined(isolate)),
          exception_state);
}

void ReadableStreamDefaultController::Error(
    const std::optional<v8::Local<v8::Value>>& e) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  // Step 1. Perform ! ReadableStreamDefaultControllerError(this, e).
  Error(isolate, this, e.value_or(v8::Undefined(isolate)));
}

// static
void ReadableStreamDefaultController::Close(
    v8::Isolate* isolate,
    ReadableStreamDefaultController* controller) {
  // Step 1. Let stream be controller.[[stream]].
  ReadableStream* const stream = controller->controlled_readable_stream_;
  // Step 2. Assert: ! ReadableStreamDefaultControllerCanCloseOrEnqueue(
  // controller) is true.
  CHECK(CanCloseOrEnqueue(controller));
  // Step 3. Set controller.[[closeRequested]] to true.
  controller->is_close_requested_ = true;
  // Step 4. If controller.[[queue]] is empty, clear the algorithms and close
  // the stream.
  if (controller->queue_->IsEmpty()) {
    ClearAlgorithms(controller);
    ReadableStream::Close(isolate, stream);
  }
}

// static
void ReadableStreamDefaultController::Enqueue(
    v8::Isolate* isolate,
    ReadableStreamDefaultController* controller,
    v8::Local<v8::Value> chunk,
    bindings::ExceptionState& exception_state) {
  // Step 1. If ! ReadableStreamDefaultControllerCanCloseOrEnqueue(controller)
  // is false, return.
  if (!CanCloseOrEnqueue(controller)) {
    return;
  }
  // Step 2. Let stream be controller.[[stream]].
  ReadableStream* const stream = controller->controlled_readable_stream_;
  // Step 3. If ! IsReadableStreamLocked(stream) is true and !
  // ReadableStreamGetNumReadRequests(stream) > 0, perform !
  // ReadableStreamFulfillReadRequest(stream, chunk, false).
  if (ReadableStream::IsLocked(stream) &&
      ReadableStream::GetNumReadRequests(stream) > 0) {
    ReadableStream::FulfillReadRequest(isolate, stream, chunk, false,
                                       exception_state);
  } else {
    // Step 4. Otherwise: let result be the result of performing
    // controller.[[strategySizeAlgorithm]], passing in chunk.
    const bindings::TryRethrowScope rethrow_scope(isolate, exception_state);
    const std::optional<double> chunk_size =
        controller->strategy_size_algorithm_->Run(isolate, chunk);
    // Step 4.b. If result is an abrupt completion, error the controller with
    // result.[[Value]] and return result.
    if (rethrow_scope.HasCaught()) {
      Error(isolate, controller, rethrow_scope.GetException());
      return;
    }
    CHECK(chunk_size.has_value());
    // Step 4.d. Let enqueueResult be EnqueueValueWithSize(controller, chunk,
    // chunkSize).
    bindings::ExceptionState pass_through(isolate);
    controller->queue_->EnqueueValueWithSize(isolate, chunk, chunk_size.value(),
                                             pass_through);
    // Step 4.e. If enqueueResult is an abrupt completion, error the controller
    // with enqueueResult.[[Value]] and return enqueueResult.
    if (rethrow_scope.HasCaught()) {
      Error(isolate, controller, rethrow_scope.GetException());
      return;
    }
  }
  // Step 5. Perform ! ReadableStreamDefaultControllerCallPullIfNeeded(
  // controller).
  CallPullIfNeeded(isolate, controller);
}

// static
void ReadableStreamDefaultController::Error(
    v8::Isolate* isolate,
    ReadableStreamDefaultController* controller,
    v8::Local<v8::Value> e) {
  // Step 1. Let stream be controller.[[stream]].
  ReadableStream* const stream = controller->controlled_readable_stream_;
  // Step 2. If stream.[[state]] is not "readable", return.
  if (stream->state_ != ReadableStream::kReadable) {
    return;
  }
  // Step 3. Perform ! ResetQueue(controller).
  controller->queue_->ResetQueue();
  // Step 4. Perform ! ReadableStreamDefaultControllerClearAlgorithms(
  // controller).
  ClearAlgorithms(controller);
  // Step 5. Perform ! ReadableStreamError(stream, e).
  ReadableStream::Error(isolate, stream, e);
}

std::optional<double> ReadableStreamDefaultController::GetDesiredSize() const {
  switch (controlled_readable_stream_->state_) {
    case ReadableStream::kErrored:
      // Step 2. If state is "errored", return null.
      return std::nullopt;
    case ReadableStream::kClosed:
      // Step 3. If state is "closed", return 0.
      return 0.0;
    case ReadableStream::kReadable:
      // Step 4. Return controller.[[strategyHWM]] −
      // controller.[[queueTotalSize]].
      return strategy_high_water_mark_ - queue_->TotalSize();
  }
  NOTREACHED();
}

// static
bool ReadableStreamDefaultController::CanCloseOrEnqueue(
    const ReadableStreamDefaultController* controller) {
  // Steps 1-3. Return true if controller.[[closeRequested]] is false and the
  // stream's state is "readable", and otherwise false.
  return !controller->is_close_requested_ &&
         controller->controlled_readable_stream_->state_ ==
             ReadableStream::kReadable;
}

// static
bool ReadableStreamDefaultController::HasBackpressure(
    const ReadableStreamDefaultController* controller) {
  // Steps 1-2. Return false if ! ReadableStreamDefaultControllerShouldCallPull(
  // controller) is true, and otherwise true.
  return !ShouldCallPull(controller);
}

// static
const char* ReadableStreamDefaultController::EnqueueExceptionMessage(
    const ReadableStreamDefaultController* controller) {
  if (controller->is_close_requested_) {
    return "Cannot enqueue a chunk into a readable stream that is closed or "
           "has been requested to be closed";
  }
  const ReadableStream::State state =
      controller->controlled_readable_stream_->state_;
  if (state == ReadableStream::kErrored) {
    return "Cannot enqueue a chunk into an errored readable stream";
  }
  CHECK_EQ(state, ReadableStream::kClosed);
  return "Cannot enqueue a chunk into a closed readable stream";
}

void ReadableStreamDefaultController::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(cancel_algorithm_);
  visitor->Trace(controlled_readable_stream_);
  visitor->Trace(pull_algorithm_);
  visitor->Trace(queue_);
  visitor->Trace(strategy_size_algorithm_);
  visitor->Trace(resolve_function_);
  visitor->Trace(reject_function_);
  ReadableStreamController::Trace(visitor);
}

v8::Local<v8::Promise> ReadableStreamDefaultController::CancelSteps(
    v8::Isolate* isolate,
    v8::Local<v8::Value> reason) {
  // Step 1. Perform ! ResetQueue(this).
  queue_->ResetQueue();
  // Step 2. Let result be the result of performing this.[[cancelAlgorithm]],
  // passing reason.
  const v8::Local<v8::Promise> result =
      cancel_algorithm_->Run(isolate, base::span_from_ref(reason));
  // Step 3. Perform ! ReadableStreamDefaultControllerClearAlgorithms(this).
  ClearAlgorithms(this);
  // Step 4. Return result.
  return result;
}

void ReadableStreamDefaultController::PullSteps(
    v8::Isolate* isolate,
    ReadRequest* read_request,
    bindings::ExceptionState& exception_state) {
  // Step 1. Let stream be this.[[stream]].
  ReadableStream* const stream = controlled_readable_stream_;
  // Step 2. If this.[[queue]] is not empty:
  if (!queue_->IsEmpty()) {
    // Step 2.a. Let chunk be ! DequeueValue(this).
    const v8::Local<v8::Value> chunk = queue_->DequeueValue(isolate);
    // Step 2.b. If this.[[closeRequested]] is true and this.[[queue]] is
    // empty, clear the algorithms and close the stream.
    if (is_close_requested_ && queue_->IsEmpty()) {
      ClearAlgorithms(this);
      ReadableStream::Close(isolate, stream);
    } else {
      // Step 2.c. Otherwise, perform !
      // ReadableStreamDefaultControllerCallPullIfNeeded(this).
      CallPullIfNeeded(isolate, this);
    }
    // Step 2.d. Perform readRequest's chunk steps, given chunk.
    read_request->ChunkSteps(isolate, chunk, exception_state);
  } else {
    // Step 3. Otherwise: perform ! ReadableStreamAddReadRequest(stream,
    // readRequest), and ! ReadableStreamDefaultControllerCallPullIfNeeded(
    // this).
    ReadableStream::AddReadRequest(isolate, stream, read_request);
    CallPullIfNeeded(isolate, this);
  }
}

void ReadableStreamDefaultController::ReleaseSteps() {
  // Step 1. Return.
}

// static
void ReadableStreamDefaultController::CallPullIfNeeded(
    v8::Isolate* isolate,
    ReadableStreamDefaultController* controller) {
  // Steps 1-2. If ! ReadableStreamDefaultControllerShouldCallPull(controller)
  // is false, return.
  if (!ShouldCallPull(controller)) {
    return;
  }
  // Step 3. If controller.[[pulling]] is true, set controller.[[pullAgain]]
  // to true and return.
  if (controller->is_pulling_) {
    controller->will_pull_again_ = true;
    return;
  }
  // Step 4. Assert: controller.[[pullAgain]] is false.
  CHECK(!controller->will_pull_again_);
  // Step 5. Set controller.[[pulling]] to true.
  controller->is_pulling_ = true;
  // Step 6. Let pullPromise be the result of performing
  // controller.[[pullAlgorithm]].
  const v8::Local<v8::Promise> pull_promise =
      controller->pull_algorithm_->Run(isolate, {});
  // Steps 7-8. React to pullPromise.
  bindings::PromiseThen(isolate, pull_promise,
                        controller->resolve_function_.Get(),
                        controller->reject_function_.Get());
}

// static
bool ReadableStreamDefaultController::ShouldCallPull(
    const ReadableStreamDefaultController* controller) {
  // Step 1. Let stream be controller.[[stream]].
  const ReadableStream* const stream = controller->controlled_readable_stream_;
  // Step 2. If ! ReadableStreamDefaultControllerCanCloseOrEnqueue(controller)
  // is false, return false.
  if (!CanCloseOrEnqueue(controller)) {
    return false;
  }
  // Step 3. If controller.[[started]] is false, return false.
  if (!controller->is_started_) {
    return false;
  }
  // Step 4. If ! IsReadableStreamLocked(stream) is true and !
  // ReadableStreamGetNumReadRequests(stream) > 0, return true.
  if (ReadableStream::IsLocked(stream) &&
      ReadableStream::GetNumReadRequests(stream) > 0) {
    return true;
  }
  // Steps 5-8. Return true if the desired size, which is not null, is above
  // 0.
  const std::optional<double> desired_size = controller->GetDesiredSize();
  CHECK(desired_size.has_value());
  return desired_size.value() > 0;
}

// static
void ReadableStreamDefaultController::ClearAlgorithms(
    ReadableStreamDefaultController* controller) {
  // Steps 1-3. Set the pull, cancel and size algorithms to undefined.
  controller->pull_algorithm_ = nullptr;
  controller->cancel_algorithm_ = nullptr;
  controller->strategy_size_algorithm_ = nullptr;
}

// static
void ReadableStreamDefaultController::SetUp(
    v8::Isolate* isolate,
    ReadableStream* stream,
    ReadableStreamDefaultController* controller,
    StreamStartAlgorithm* start_algorithm,
    StreamAlgorithm* pull_algorithm,
    StreamAlgorithm* cancel_algorithm,
    double high_water_mark,
    StrategySizeAlgorithm* size_algorithm,
    bindings::ExceptionState& exception_state) {
  // Step 1. Assert: stream.[[controller]] is undefined.
  CHECK(!stream->readable_stream_controller_);
  // Step 2. Set controller.[[stream]] to stream.
  controller->controlled_readable_stream_ = stream;
  // Step 3. The constructor reset the queue.
  CHECK(controller->queue_->IsEmpty());
  // Step 5. Set controller.[[strategySizeAlgorithm]] to sizeAlgorithm and
  // controller.[[strategyHWM]] to highWaterMark.
  controller->strategy_size_algorithm_ = size_algorithm;
  controller->strategy_high_water_mark_ = high_water_mark;
  // Steps 6-7. Set controller.[[pullAlgorithm]] and
  // controller.[[cancelAlgorithm]].
  controller->pull_algorithm_ = pull_algorithm;
  controller->cancel_algorithm_ = cancel_algorithm;
  // Step 8. Set stream.[[controller]] to controller.
  stream->readable_stream_controller_ = controller;
  // Steps 9-10. Let startResult be the result of performing startAlgorithm,
  // which may throw, and startPromise a promise resolved with it, which the
  // algorithm makes.
  const bindings::TryRethrowScope rethrow_scope(isolate, exception_state);
  const v8::Local<v8::Promise> start_promise = start_algorithm->Run(isolate);
  if (start_promise.IsEmpty()) {
    CHECK(rethrow_scope.HasCaught());
    return;
  }

  class ResolveFunction final : public bindings::ScriptFunction {
   public:
    explicit ResolveFunction(ReadableStreamDefaultController* controller)
        : controller_(controller) {}

    v8::Local<v8::Value> Call(v8::Isolate* isolate,
                              v8::Local<v8::Value>) override {
      // Step 11. Upon fulfillment of startPromise: set controller.[[started]]
      // to true, and call pull if needed.
      controller_->is_started_ = true;
      CHECK(!controller_->is_pulling_);
      CHECK(!controller_->will_pull_again_);
      CallPullIfNeeded(isolate, controller_);
      return v8::Undefined(isolate);
    }

    void Trace(cppgc::Visitor* visitor) const override {
      visitor->Trace(controller_);
      bindings::ScriptFunction::Trace(visitor);
    }

   private:
    const cppgc::Member<ReadableStreamDefaultController> controller_;
  };

  class RejectFunction final : public bindings::ScriptFunction {
   public:
    explicit RejectFunction(ReadableStreamDefaultController* controller)
        : controller_(controller) {}

    v8::Local<v8::Value> Call(v8::Isolate* isolate,
                              v8::Local<v8::Value> r) override {
      // Step 12. Upon rejection of startPromise with reason r, perform !
      // ReadableStreamDefaultControllerError(controller, r).
      Error(isolate, controller_, r);
      return v8::Undefined(isolate);
    }

    void Trace(cppgc::Visitor* visitor) const override {
      visitor->Trace(controller_);
      bindings::ScriptFunction::Trace(visitor);
    }

   private:
    const cppgc::Member<ReadableStreamDefaultController> controller_;
  };

  bindings::PromiseThen(
      isolate, start_promise,
      bindings::MakeGarbageCollected<ResolveFunction>(controller),
      bindings::MakeGarbageCollected<RejectFunction>(controller));
}

// static
void ReadableStreamDefaultController::SetUpFromUnderlyingSource(
    v8::Isolate* isolate,
    ReadableStream* stream,
    v8::Local<v8::Object> underlying_source,
    double high_water_mark,
    StrategySizeAlgorithm* size_algorithm,
    bindings::ExceptionState& exception_state) {
  // Step 1. Let controller be a new ReadableStreamDefaultController.
  auto* const controller =
      bindings::MakeGarbageCollected<ReadableStreamDefaultController>();
  const v8::Local<v8::Value> controller_value = controller->ToV8(isolate);
  // Step 2. Let startAlgorithm be the steps that return ?
  // InvokeOrNoop(underlyingSource, "start", « controller »).
  StreamStartAlgorithm* const start_algorithm = CreateStartAlgorithm(
      isolate, underlying_source, "underlyingSource.start", controller_value);
  // Step 3. Let pullAlgorithm be ? CreateAlgorithmFromUnderlyingMethod(
  // underlyingSource, "pull", 0, « controller »).
  StreamAlgorithm* const pull_algorithm = CreateAlgorithmFromUnderlyingMethod(
      isolate, underlying_source, "pull", "underlyingSource.pull",
      controller_value, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  // Step 4. Let cancelAlgorithm be ? CreateAlgorithmFromUnderlyingMethod(
  // underlyingSource, "cancel", 1, « »).
  StreamAlgorithm* const cancel_algorithm = CreateAlgorithmFromUnderlyingMethod(
      isolate, underlying_source, "cancel", "underlyingSource.cancel",
      controller_value, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  // Step 5. Perform ? SetUpReadableStreamDefaultController(stream, controller,
  // startAlgorithm, pullAlgorithm, cancelAlgorithm, highWaterMark,
  // sizeAlgorithm).
  SetUp(isolate, stream, controller, start_algorithm, pull_algorithm,
        cancel_algorithm, high_water_mark, size_algorithm, exception_state);
}

}  // namespace mitto
