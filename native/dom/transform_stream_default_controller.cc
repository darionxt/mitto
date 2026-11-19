#include "mitto/dom/transform_stream_default_controller.h"

#include <optional>

#include "base/check.h"
#include "base/check_op.h"
#include "base/containers/span.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_function.h"
#include "mitto/bindings/script_promise.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_transform_stream_default_controller.h"
#include "mitto/dom/miscellaneous_operations.h"
#include "mitto/dom/readable_stream.h"
#include "mitto/dom/readable_stream_controller.h"
#include "mitto/dom/readable_stream_default_controller.h"
#include "mitto/dom/stream_algorithms.h"
#include "mitto/dom/transform_stream.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

TransformStreamDefaultController::TransformStreamDefaultController() = default;
TransformStreamDefaultController::~TransformStreamDefaultController() = default;

const bindings::WrapperTypeInfo*
TransformStreamDefaultController::GetWrapperTypeInfo() const {
  return &bindings::V8TransformStreamDefaultController::kWrapperTypeInfo;
}

// static
ReadableStreamDefaultController*
TransformStreamDefaultController::GetDefaultController(
    TransformStream* stream) {
  // The TransformStreamDefaultController will always use a
  // ReadableStreamDefaultController. Hence, it is safe to down-cast here.
  ReadableStreamController* const controller =
      stream->readable_->GetController();
  CHECK(controller->IsDefaultController());
  return static_cast<ReadableStreamDefaultController*>(controller);
}

std::optional<double> TransformStreamDefaultController::DesiredSize() const {
  // https://streams.spec.whatwg.org/#ts-default-controller-desired-size
  // 2. Let readableController be
  //    this.[[controlledTransformStream]].[[readable]].
  //    [[readableStreamController]].
  const ReadableStreamDefaultController* const readable_controller =
      GetDefaultController(controlled_transform_stream_);

  // 3. Return !
  //    ReadableStreamDefaultControllerGetDesiredSize(readableController).
  // Use the accessor instead as it already has the semantics we need and can't
  // be interfered with from JavaScript.
  return readable_controller->DesiredSize();
}

// The handling of undefined arguments is implicit in the standard, but needs to
// be done explicitly with IDL.
void TransformStreamDefaultController::Enqueue(
    const std::optional<v8::Local<v8::Value>>& chunk,
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  // https://streams.spec.whatwg.org/#ts-default-controller-enqueue
  // 2. Perform ? TransformStreamDefaultControllerEnqueue(this, chunk).
  Enqueue(isolate, this, chunk.value_or(v8::Undefined(isolate)),
          exception_state);
}

void TransformStreamDefaultController::Error(
    const std::optional<v8::Local<v8::Value>>& reason) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  // https://streams.spec.whatwg.org/#ts-default-controller-error
  // 2. Perform ! TransformStreamDefaultControllerError(this, reason).
  Error(isolate, this, reason.value_or(v8::Undefined(isolate)));
}

void TransformStreamDefaultController::Terminate() {
  // https://streams.spec.whatwg.org/#ts-default-controller-terminate
  // 2. Perform ! TransformStreamDefaultControllerTerminate(this).
  Terminate(v8::Isolate::GetCurrent(), this);
}

void TransformStreamDefaultController::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(controlled_transform_stream_);
  visitor->Trace(flush_algorithm_);
  visitor->Trace(transform_algorithm_);
  visitor->Trace(reject_function_);
  bindings::ScriptWrappable::Trace(visitor);
}

// This algorithm is not explicitly named, but is described as part of the
// SetUpTransformStreamDefaultControllerFromTransformer abstract operation in
// the standard.
class TransformStreamDefaultController::DefaultTransformAlgorithm final
    : public StreamAlgorithm {
 public:
  explicit DefaultTransformAlgorithm(
      TransformStreamDefaultController* controller)
      : controller_(controller) {}

  v8::Local<v8::Promise> Run(v8::Isolate* isolate,
                             base::span<v8::Local<v8::Value>> argv) override {
    CHECK_EQ(argv.size(), 1u);
    v8::TryCatch try_catch(isolate);

    // https://streams.spec.whatwg.org/#set-up-transform-stream-default-controller-from-transformer
    // 3. Let transformAlgorithm be the following steps, taking a chunk
    //    argument:
    //    a. Let result be TransformStreamDefaultControllerEnqueue(controller,
    //       chunk).
    {
      bindings::ExceptionState pass_through(isolate);
      Enqueue(isolate, controller_, argv[0], pass_through);
    }

    //    b. If result is an abrupt completion, return a promise rejected with
    //       result.[[Value]].
    if (try_catch.HasCaught()) {
      return bindings::RejectedPromise(isolate, try_catch.Exception());
    }

    //    c. Otherwise, return a promise resolved with undefined.
    return bindings::ResolvedUndefinedPromise(isolate);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(controller_);
    StreamAlgorithm::Trace(visitor);
  }

 private:
  cppgc::Member<TransformStreamDefaultController> controller_;
};

class TransformStreamDefaultController::PerformTransformRejectFunction final
    : public bindings::ScriptFunction {
 public:
  explicit PerformTransformRejectFunction(TransformStream* stream)
      : stream_(stream) {}

  v8::Local<v8::Value> Call(v8::Isolate* isolate,
                            v8::Local<v8::Value> r) override {
    // 2. Return the result of transforming transformPromise with a rejection
    //    handler that, when called with argument r, performs the following
    //    steps:
    //    a. Perform ! TransformStreamError(controller.
    //       [[controlledTransformStream]], r).
    TransformStream::Error(isolate, stream_, r);

    //    b. Throw r.
    isolate->ThrowException(r);
    return {};
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(stream_);
    bindings::ScriptFunction::Trace(visitor);
  }

 private:
  cppgc::Member<TransformStream> stream_;
};

// static
void TransformStreamDefaultController::SetUp(
    v8::Isolate* isolate,
    TransformStream* stream,
    TransformStreamDefaultController* controller,
    StreamAlgorithm* transform_algorithm,
    StreamAlgorithm* flush_algorithm) {
  // https://streams.spec.whatwg.org/#set-up-transform-stream-default-controller
  // 1. Assert: ! IsTransformStream(stream) is true.
  CHECK(stream);

  // 2. Assert: stream.[[transformStreamController]] is undefined.
  CHECK(!stream->transform_stream_controller_);

  // 3. Set controller.[[controlledTransformStream]] to stream.
  controller->controlled_transform_stream_ = stream;

  // 4. Set stream.[[transformStreamController]] to controller.
  stream->transform_stream_controller_ = controller;

  // 5. Set controller.[[transformAlgorithm]] to transformAlgorithm.
  controller->transform_algorithm_ = transform_algorithm;

  // 6. Set controller.[[flushAlgorithm]] to flushAlgorithm.
  controller->flush_algorithm_ = flush_algorithm;

  controller->reject_function_ =
      bindings::MakeGarbageCollected<PerformTransformRejectFunction>(
          controller->controlled_transform_stream_.Get());
}

// static
v8::Local<v8::Value> TransformStreamDefaultController::SetUpFromTransformer(
    v8::Isolate* isolate,
    TransformStream* stream,
    v8::Local<v8::Object> transformer,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#set-up-transform-stream-default-controller-from-transformer
  // 1. Assert: transformer is not undefined.

  // 2. Let controller be ObjectCreate(the original value of
  //    TransformStreamDefaultController's prototype property).
  auto* const controller =
      bindings::MakeGarbageCollected<TransformStreamDefaultController>();

  const v8::Local<v8::Value> controller_value = controller->ToV8(isolate);

  // The following steps are reordered from the standard for efficiency, but the
  // effect is the same.
  StreamAlgorithm* transform_algorithm = nullptr;

  // 4. Let transformMethod be ? GetV(transformer, "transform").
  const v8::MaybeLocal<v8::Value> method_maybe =
      ResolveMethod(isolate, transformer, "transform", "transformer.transform",
                    exception_state);
  v8::Local<v8::Value> transform_method;
  if (!method_maybe.ToLocal(&transform_method)) {
    CHECK(exception_state.HadException());
    return v8::Local<v8::Value>();
  }
  CHECK(!exception_state.HadException());

  if (transform_method->IsUndefined()) {
    // 3. Let transformAlgorithm be the following steps, taking a chunk
    // argument:
    //    i. Let result be TransformStreamDefaultControllerEnqueue(controller,
    //       chunk).
    //   ii. If result is an abrupt completion, return a promise rejected with
    //       result.[[Value]].
    //  iii. Otherwise, return a promise resolved with undefined.
    transform_algorithm =
        bindings::MakeGarbageCollected<DefaultTransformAlgorithm>(controller);
  } else {
    // 5. If transformMethod is not undefined,
    //    a. If ! IsCallable(transformMethod) is false, throw a TypeError
    //       exception.
    // (The IsCallable() check has already been done by ResolveMethod).

    //    b. Set transformAlgorithm to the following steps, taking a chunk
    //       argument:
    //       i. Return ! PromiseCall(transformMethod, transformer, « chunk,
    //          controller »).
    transform_algorithm = CreateAlgorithmFromResolvedMethod(
        isolate, transformer, transform_method, controller_value);
  }

  // 6. Let flushAlgorithm be ? CreateAlgorithmFromUnderlyingMethod(transformer,
  //    "flush", 0, « controller »).
  StreamAlgorithm* const flush_algorithm = CreateAlgorithmFromUnderlyingMethod(
      isolate, transformer, "flush", "transformer.flush", controller_value,
      exception_state);

  // 7. Perform ! SetUpTransformStreamDefaultController(stream, controller,
  //    transformAlgorithm, flushAlgorithm).
  SetUp(isolate, stream, controller, transform_algorithm, flush_algorithm);

  // This operation doesn't have a return value in the standard, but it's useful
  // to return the JavaScript wrapper here so that it can be used when calling
  // transformer.start().
  return controller_value;
}

// static
void TransformStreamDefaultController::ClearAlgorithms(
    TransformStreamDefaultController* controller) {
  // https://streams.spec.whatwg.org/#transform-stream-default-controller-clear-algorithms
  // 1. Set controller.[[transformAlgorithm]] to undefined.
  controller->transform_algorithm_ = nullptr;

  // 2. Set controller.[[flushAlgorithm]] to undefined.
  controller->flush_algorithm_ = nullptr;
}

// static
void TransformStreamDefaultController::Enqueue(
    v8::Isolate* isolate,
    TransformStreamDefaultController* controller,
    v8::Local<v8::Value> chunk,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#transform-stream-default-controller-enqueue
  // 1. Let stream be controller.[[controlledTransformStream]].
  TransformStream* const stream = controller->controlled_transform_stream_;

  // 2. Let readableController be
  //    stream.[[readable]].[[readableStreamController]].
  ReadableStreamDefaultController* const readable_controller =
      GetDefaultController(stream);

  // 3. If !
  //    ReadableStreamDefaultControllerCanCloseOrEnqueue(readableController) is
  //    false, throw a TypeError exception.
  if (!ReadableStreamDefaultController::CanCloseOrEnqueue(
          readable_controller)) {
    exception_state.ThrowTypeError(
        ReadableStreamDefaultController::EnqueueExceptionMessage(
            readable_controller));
    return;
  }

  // 4. Let enqueueResult be ReadableStreamDefaultControllerEnqueue(
  //    readableController, chunk).
  const bindings::TryRethrowScope rethrow_scope(isolate, exception_state);
  v8::Local<v8::Value> enqueue_error;
  {
    v8::TryCatch try_catch(isolate);
    bindings::ExceptionState pass_through(isolate);
    ReadableStreamDefaultController::Enqueue(isolate, readable_controller,
                                             chunk, pass_through);
    if (try_catch.HasCaught()) {
      enqueue_error = try_catch.Exception();
    }
  }

  // 5. If enqueueResult is an abrupt completion,
  if (!enqueue_error.IsEmpty()) {
    // a. Perform ! TransformStreamErrorWritableAndUnblockWrite(stream,
    //    enqueueResult.[[Value]]).
    TransformStream::ErrorWritableAndUnblockWrite(isolate, stream,
                                                  enqueue_error);

    // b. Throw stream.[[readable]].[[storedError]].
    isolate->ThrowException(stream->readable_->GetStoredError(isolate));
    return;
  }

  // 6. Let backpressure be ! ReadableStreamDefaultControllerHasBackpressure(
  //    readableController).
  const bool backpressure =
      ReadableStreamDefaultController::HasBackpressure(readable_controller);

  // 7. If backpressure is not stream.[[backpressure]],
  if (backpressure != stream->had_backpressure_) {
    // a. Assert: backpressure is true.
    CHECK(backpressure);

    // b. Perform ! TransformStreamSetBackpressure(stream, true).
    TransformStream::SetBackpressure(isolate, stream, true);
  }
}

// static
void TransformStreamDefaultController::Error(
    v8::Isolate* isolate,
    TransformStreamDefaultController* controller,
    v8::Local<v8::Value> e) {
  // https://streams.spec.whatwg.org/#transform-stream-default-controller-error
  // 1. Perform ! TransformStreamError(controller.[[controlledTransformStream]],
  //    e).
  TransformStream::Error(isolate, controller->controlled_transform_stream_, e);
}

// static
v8::Local<v8::Promise> TransformStreamDefaultController::PerformTransform(
    v8::Isolate* isolate,
    TransformStreamDefaultController* controller,
    v8::Local<v8::Value> chunk) {
  // https://streams.spec.whatwg.org/#transform-stream-default-controller-perform-transform
  // 1. Let transformPromise be the result of performing controller.
  //    [[transformAlgorithm]], passing chunk.
  const v8::Local<v8::Promise> transform_promise =
      controller->transform_algorithm_->Run(isolate,
                                            base::span_from_ref(chunk));
  CHECK(!transform_promise.IsEmpty());

  // 2. Return the result of transforming transformPromise ...
  return bindings::PromiseThen(isolate, transform_promise, nullptr,
                               controller->reject_function_.Get());
}

// static
void TransformStreamDefaultController::Terminate(
    v8::Isolate* isolate,
    TransformStreamDefaultController* controller) {
  // https://streams.spec.whatwg.org/#transform-stream-default-controller-terminate
  // 1. Let stream be controller.[[controlledTransformStream]].
  TransformStream* const stream = controller->controlled_transform_stream_;

  // 2. Let readableController be
  //    stream.[[readable]].[[readableStreamController]].
  ReadableStreamDefaultController* const readable_controller =
      GetDefaultController(stream);

  // 3. If !
  //    ReadableStreamDefaultControllerCanCloseOrEnqueue(readableController) is
  //    true, perform ! ReadableStreamDefaultControllerClose(
  //    readableController).
  if (ReadableStreamDefaultController::CanCloseOrEnqueue(readable_controller)) {
    ReadableStreamDefaultController::Close(isolate, readable_controller);
  }

  // 4. Let error be a TypeError exception indicating that the stream has been
  //    terminated.
  const v8::Local<v8::Value> error =
      CreateTypeError(isolate, "The transform stream has been terminated");

  // 5. Perform ! TransformStreamErrorWritableAndUnblockWrite(stream, error).
  TransformStream::ErrorWritableAndUnblockWrite(isolate, stream, error);
}

}  // namespace mitto
