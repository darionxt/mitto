#include "mitto/dom/transform_stream.h"

#include <optional>

#include "base/check.h"
#include "base/check_op.h"
#include "base/containers/span.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_function.h"
#include "mitto/bindings/script_promise.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_transform_stream.h"
#include "mitto/dom/miscellaneous_operations.h"
#include "mitto/dom/readable_stream.h"
#include "mitto/dom/readable_stream_controller.h"
#include "mitto/dom/readable_stream_default_controller.h"
#include "mitto/dom/stream_algorithms.h"
#include "mitto/dom/transform_stream_default_controller.h"
#include "mitto/dom/transform_stream_transformer.h"
#include "mitto/dom/writable_stream.h"
#include "mitto/dom/writable_stream_default_controller.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-traced-handle.h"

namespace mitto {

// A flush algorithm that delegates to a TransformStreamTransformer, for a
// TransformStream that C++ makes.
class TransformStream::FlushAlgorithm final : public StreamAlgorithm {
 public:
  explicit FlushAlgorithm(TransformStreamTransformer* transformer)
      : transformer_(transformer) {}

  v8::Local<v8::Promise> Run(v8::Isolate* isolate,
                             base::span<v8::Local<v8::Value>> argv) override {
    CHECK_EQ(argv.size(), 0u);
    CHECK(controller_);
    v8::TryCatch try_catch(isolate);
    v8::Local<v8::Promise> promise;
    {
      bindings::ExceptionState pass_through(isolate);
      promise = transformer_->Flush(controller_, pass_through);
    }
    if (try_catch.HasCaught()) {
      return bindings::RejectedPromise(isolate, try_catch.Exception());
    }

    return promise;
  }

  // SetController() must be called before Run() is.
  void SetController(TransformStreamDefaultController* controller) {
    controller_ = controller;
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(transformer_);
    visitor->Trace(controller_);
    StreamAlgorithm::Trace(visitor);
  }

 private:
  cppgc::Member<TransformStreamTransformer> transformer_;
  cppgc::Member<TransformStreamDefaultController> controller_;
};

// A transform algorithm that delegates to a TransformStreamTransformer.
class TransformStream::TransformAlgorithm final : public StreamAlgorithm {
 public:
  explicit TransformAlgorithm(TransformStreamTransformer* transformer)
      : transformer_(transformer) {}

  v8::Local<v8::Promise> Run(v8::Isolate* isolate,
                             base::span<v8::Local<v8::Value>> argv) override {
    CHECK_EQ(argv.size(), 1u);
    CHECK(controller_);
    v8::TryCatch try_catch(isolate);
    v8::Local<v8::Promise> promise;
    {
      bindings::ExceptionState pass_through(isolate);
      promise = transformer_->Transform(argv[0], controller_, pass_through);
    }
    if (try_catch.HasCaught()) {
      return bindings::RejectedPromise(isolate, try_catch.Exception());
    }

    return promise;
  }

  // SetController() must be called before Run() is.
  void SetController(TransformStreamDefaultController* controller) {
    controller_ = controller;
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(transformer_);
    visitor->Trace(controller_);
    StreamAlgorithm::Trace(visitor);
  }

 private:
  cppgc::Member<TransformStreamTransformer> transformer_;
  cppgc::Member<TransformStreamDefaultController> controller_;
};

// static
TransformStream* TransformStream::Create(
    const std::optional<v8::Local<v8::Value>>& transformer,
    const std::optional<v8::Local<v8::Value>>& writable_strategy,
    const std::optional<v8::Local<v8::Value>>& readable_strategy,
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  const v8::Local<v8::Value> undefined = v8::Undefined(isolate);
  auto* const ts = bindings::MakeGarbageCollected<TransformStream>();

  ts->InitInternal(isolate, transformer.value_or(undefined),
                   writable_strategy.value_or(undefined),
                   readable_strategy.value_or(undefined), exception_state);

  if (exception_state.HadException()) {
    return nullptr;
  }

  return ts;
}

// static
TransformStream* TransformStream::Create(
    v8::Isolate* isolate,
    TransformStreamTransformer* transformer,
    bindings::ExceptionState& exception_state) {
  auto* const transform_algorithm =
      bindings::MakeGarbageCollected<TransformAlgorithm>(transformer);
  auto* const flush_algorithm =
      bindings::MakeGarbageCollected<FlushAlgorithm>(transformer);
  StrategySizeAlgorithm* const size_algorithm = CreateDefaultSizeAlgorithm();
  TransformStream* const stream = Create(
      isolate, CreateTrivialStartAlgorithm(), transform_algorithm,
      flush_algorithm, 1, size_algorithm, 0, size_algorithm, exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }
  CHECK(stream);
  TransformStreamDefaultController* const controller =
      stream->transform_stream_controller_;
  transform_algorithm->SetController(controller);
  flush_algorithm->SetController(controller);
  return stream;
}

// static
TransformStream* TransformStream::Create(
    v8::Isolate* isolate,
    StreamStartAlgorithm* start_algorithm,
    StreamAlgorithm* transform_algorithm,
    StreamAlgorithm* flush_algorithm,
    double writable_high_water_mark,
    StrategySizeAlgorithm* writable_size_algorithm,
    double readable_high_water_mark,
    StrategySizeAlgorithm* readable_size_algorithm,
    bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#create-transform-stream
  //
  // All the arguments are compulsory here, so steps 1 to 4, which give the
  // missing ones their defaults, are not performed.

  // 5. Assert: ! IsNonNegativeNumber(writableHighWaterMark) is true.
  CHECK_GE(writable_high_water_mark, 0);

  // 6. Assert: ! IsNonNegativeNumber(readableHighWaterMark) is true.
  CHECK_GE(readable_high_water_mark, 0);

  // 7. Let stream be ObjectCreate(the original value of TransformStream's
  //    prototype property).
  auto* const stream = bindings::MakeGarbageCollected<TransformStream>();

  // 8. Let startPromise be a new promise.
  auto* const start_promise =
      bindings::MakeGarbageCollected<bindings::ScriptPromiseResolver>(isolate);

  // 9. Perform ! InitializeTransformStream(stream, startPromise,
  //    writableHighWaterMark, writableSizeAlgorithm, readableHighWaterMark,
  //    readableSizeAlgorithm).
  Initialize(isolate, stream, start_promise, writable_high_water_mark,
             writable_size_algorithm, readable_high_water_mark,
             readable_size_algorithm, exception_state);

  if (exception_state.HadException()) {
    return nullptr;
  }
  // 10. Let controller be ObjectCreate(the original value of
  //     TransformStreamDefaultController's prototype property).
  auto* const controller =
      bindings::MakeGarbageCollected<TransformStreamDefaultController>();

  // 11. Perform ! SetUpTransformStreamDefaultController(stream, controller,
  //     transformAlgorithm, flushAlgorithm).
  TransformStreamDefaultController::SetUp(isolate, stream, controller,
                                          transform_algorithm, flush_algorithm);

  // 12. Let startResult be the result of performing startAlgorithm. (This may
  //     throw an exception.)
  v8::Local<v8::Promise> start_result;
  {
    const bindings::TryRethrowScope rethrow_scope(isolate, exception_state);
    start_result = start_algorithm->Run(isolate);
    if (start_result.IsEmpty()) {
      CHECK(rethrow_scope.HasCaught());
      return nullptr;
    }
  }
  // 13. Resolve startPromise with startResult.
  start_promise->Resolve(isolate, start_result);

  // 14. Return stream.
  return stream;
}

// Only used internally.
TransformStream::TransformStream() = default;

TransformStream::TransformStream(ReadableStream* readable,
                                 WritableStream* writable)
    : readable_(readable), writable_(writable) {}

TransformStream::~TransformStream() = default;

const bindings::WrapperTypeInfo* TransformStream::GetWrapperTypeInfo() const {
  return &bindings::V8TransformStream::kWrapperTypeInfo;
}

ReadableStreamDefaultController* TransformStream::GetReadableController() {
  // The type of source is not given when constructing the readable stream in
  // TransformStream, so it is guaranteed that the controller is a
  // ReadableStreamDefaultController.
  ReadableStreamController* const controller = readable_->GetController();
  CHECK(controller->IsDefaultController());
  return static_cast<ReadableStreamDefaultController*>(controller);
}

void TransformStream::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(backpressure_change_promise_);
  visitor->Trace(readable_);
  visitor->Trace(transform_stream_controller_);
  visitor->Trace(writable_);
  bindings::ScriptWrappable::Trace(visitor);
}

// The "an algorithm that returns startPromise" step of
// InitializeTransformStream():
// https://streams.spec.whatwg.org/#initialize-transform-stream.
class TransformStream::ReturnStartPromiseAlgorithm final
    : public StreamStartAlgorithm {
 public:
  explicit ReturnStartPromiseAlgorithm(
      bindings::ScriptPromiseResolver* start_promise)
      : start_promise_(start_promise) {}

  v8::Local<v8::Promise> Run(v8::Isolate* isolate) override {
    return start_promise_->Promise(isolate);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(start_promise_);
    StreamStartAlgorithm::Trace(visitor);
  }

 private:
  cppgc::Member<bindings::ScriptPromiseResolver> start_promise_;
};

//
// The following algorithms are described as abstract operations in the
// standard, but are classes here so that the collector can trace what they
// hold.
//
class TransformStream::DefaultSinkWriteAlgorithm final
    : public StreamAlgorithm {
 public:
  explicit DefaultSinkWriteAlgorithm(TransformStream* stream)
      : stream_(stream) {}

  v8::Local<v8::Promise> Run(v8::Isolate* isolate,
                             base::span<v8::Local<v8::Value>> argv) override {
    CHECK_EQ(argv.size(), 1u);
    const v8::Local<v8::Value> chunk = argv[0];

    // https://streams.spec.whatwg.org/#transform-stream-default-sink-write-algorithm
    // 1. Assert: stream.[[writable]].[[state]] is "writable".
    CHECK(stream_->writable_->IsWritable());

    // 2. Let controller be stream.[[transformStreamController]].
    TransformStreamDefaultController* const controller =
        stream_->transform_stream_controller_;

    // 3. If stream.[[backpressure]] is true,
    if (stream_->had_backpressure_) {
      // a. Let backpressureChangePromise be
      //    stream.[[backpressureChangePromise]].
      bindings::ScriptPromiseResolver* const backpressure_change_promise =
          stream_->backpressure_change_promise_.Get();

      // b. Assert: backpressureChangePromise is not undefined.
      CHECK(backpressure_change_promise);

      class ResponseFunction final : public bindings::ScriptFunction {
       public:
        ResponseFunction(v8::Isolate* isolate,
                         TransformStream* stream,
                         v8::Local<v8::Value> chunk)
            : stream_(stream), chunk_(isolate, chunk) {}

        v8::Local<v8::Value> Call(v8::Isolate* isolate,
                                  v8::Local<v8::Value>) override {
          // c. Return the result of transforming backpressureChangePromise with
          //    a fulfillment handler which performs the following steps:
          //    i. Let writable be stream.[[writable]].
          WritableStream* const writable = stream_->writable_;

          //   ii. Let state be writable.[[state]].
          //  iii. If state is "erroring", throw writable.[[storedError]].
          if (writable->IsErroring()) {
            return bindings::RejectedPromise(isolate,
                                             writable->GetStoredError(isolate));
          }

          // 4. Assert: state is "writable".
          CHECK(writable->IsWritable());

          // 5. Return ! TransformStreamDefaultControllerPerformTransform(
          //    controller, chunk).
          return TransformStreamDefaultController::PerformTransform(
              isolate, stream_->transform_stream_controller_,
              chunk_.Get(isolate));
        }

        void Trace(cppgc::Visitor* visitor) const override {
          visitor->Trace(stream_);
          visitor->Trace(chunk_);
          bindings::ScriptFunction::Trace(visitor);
        }

       private:
        cppgc::Member<TransformStream> stream_;
        v8::TracedReference<v8::Value> chunk_;
      };

      // c. Return the result of transforming backpressureChangePromise ...
      return bindings::PromiseThen(
          isolate, backpressure_change_promise->Promise(isolate),
          bindings::MakeGarbageCollected<ResponseFunction>(
              isolate, stream_.Get(), chunk));
    }

    //  4. Return ! TransformStreamDefaultControllerPerformTransform(controller,
    //     chunk).
    return TransformStreamDefaultController::PerformTransform(
        isolate, controller, chunk);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(stream_);
    StreamAlgorithm::Trace(visitor);
  }

 private:
  cppgc::Member<TransformStream> stream_;
};

class TransformStream::DefaultSinkAbortAlgorithm final
    : public StreamAlgorithm {
 public:
  explicit DefaultSinkAbortAlgorithm(TransformStream* stream)
      : stream_(stream) {}

  v8::Local<v8::Promise> Run(v8::Isolate* isolate,
                             base::span<v8::Local<v8::Value>> argv) override {
    CHECK_EQ(argv.size(), 1u);
    const v8::Local<v8::Value> reason = argv[0];

    // https://streams.spec.whatwg.org/#transform-stream-default-sink-abort-algorithm
    // 1. Perform ! TransformStreamError(stream, reason).
    Error(isolate, stream_, reason);

    // 2. Return a promise resolved with undefined.
    return bindings::ResolvedUndefinedPromise(isolate);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(stream_);
    StreamAlgorithm::Trace(visitor);
  }

 private:
  cppgc::Member<TransformStream> stream_;
};

class TransformStream::DefaultSinkCloseAlgorithm final
    : public StreamAlgorithm {
 public:
  explicit DefaultSinkCloseAlgorithm(TransformStream* stream)
      : stream_(stream) {}

  v8::Local<v8::Promise> Run(v8::Isolate* isolate,
                             base::span<v8::Local<v8::Value>> argv) override {
    CHECK_EQ(argv.size(), 0u);
    // https://streams.spec.whatwg.org/#transform-stream-default-sink-close-algorithm
    // 1. Let readable be stream.[[readable]].
    // 2. Let controller be stream.[[transformStreamController]].
    TransformStreamDefaultController* const controller =
        stream_->transform_stream_controller_;

    // 3. Let flushPromise be the result of performing
    //    controller.[[flushAlgorithm]].
    const v8::Local<v8::Promise> flush_promise =
        controller->flush_algorithm_->Run(isolate, {});

    // 4. Perform ! TransformStreamDefaultControllerClearAlgorithms(controller).
    TransformStreamDefaultController::ClearAlgorithms(controller);

    class ResolveFunction final : public bindings::ScriptFunction {
     public:
      explicit ResolveFunction(TransformStream* stream) : stream_(stream) {}

      v8::Local<v8::Value> Call(v8::Isolate* isolate,
                                v8::Local<v8::Value>) override {
        // 5. Return the result of transforming flushPromise with:
        //    a. A fulfillment handler that performs the following steps:
        //       i. If readable.[[state]] is "errored", throw
        //          readable.[[storedError]].
        if (stream_->readable_->IsErrored()) {
          isolate->ThrowException(stream_->readable_->GetStoredError(isolate));
          return {};
        }

        //      ii. Let readableController be
        //          readable.[[readableStreamController]].
        ReadableStreamDefaultController* const readable_controller =
            stream_->GetReadableController();

        //     iii. If ! ReadableStreamDefaultControllerCanCloseOrEnqueue(
        //          readableController) is true, perform !
        //          ReadableStreamDefaultControllerClose(readableController).
        if (ReadableStreamDefaultController::CanCloseOrEnqueue(
                readable_controller)) {
          ReadableStreamDefaultController::Close(isolate, readable_controller);
        }
        return v8::Undefined(isolate);
      }

      void Trace(cppgc::Visitor* visitor) const override {
        visitor->Trace(stream_);
        bindings::ScriptFunction::Trace(visitor);
      }

     private:
      cppgc::Member<TransformStream> stream_;
    };

    class RejectFunction final : public bindings::ScriptFunction {
     public:
      explicit RejectFunction(TransformStream* stream) : stream_(stream) {}

      v8::Local<v8::Value> Call(v8::Isolate* isolate,
                                v8::Local<v8::Value> r) override {
        // b. A rejection handler that, when called with argument r, performs
        //    the following steps:
        //    i. Perform ! TransformStreamError(stream, r).
        TransformStream::Error(isolate, stream_, r);

        //   ii. Throw readable.[[storedError]].
        isolate->ThrowException(stream_->readable_->GetStoredError(isolate));
        return {};
      }

      void Trace(cppgc::Visitor* visitor) const override {
        visitor->Trace(stream_);
        bindings::ScriptFunction::Trace(visitor);
      }

     private:
      cppgc::Member<TransformStream> stream_;
    };

    // 5. Return the result of transforming flushPromise ...
    return bindings::PromiseThen(
        isolate, flush_promise,
        bindings::MakeGarbageCollected<ResolveFunction>(stream_.Get()),
        bindings::MakeGarbageCollected<RejectFunction>(stream_.Get()));
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(stream_);
    StreamAlgorithm::Trace(visitor);
  }

 private:
  cppgc::Member<TransformStream> stream_;
};

class TransformStream::DefaultSourcePullAlgorithm final
    : public StreamAlgorithm {
 public:
  explicit DefaultSourcePullAlgorithm(TransformStream* stream)
      : stream_(stream) {}

  v8::Local<v8::Promise> Run(v8::Isolate* isolate,
                             base::span<v8::Local<v8::Value>> argv) override {
    CHECK_EQ(argv.size(), 0u);

    // https://streams.spec.whatwg.org/#transform-stream-default-source-pull
    // 1. Assert: stream.[[backpressure]] is true.
    CHECK(stream_->had_backpressure_);

    // 2. Assert: stream.[[backpressureChangePromise]] is not undefined.
    CHECK(stream_->backpressure_change_promise_);

    // 3. Perform ! TransformStreamSetBackpressure(stream, false).
    SetBackpressure(isolate, stream_, false);

    // 4. Return stream.[[backpressureChangePromise]].
    return stream_->backpressure_change_promise_->Promise(isolate);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(stream_);
    StreamAlgorithm::Trace(visitor);
  }

 private:
  cppgc::Member<TransformStream> stream_;
};

// This algorithm isn't explicitly named in the standard, but instead is
// described by a step in InitializeTransformStream.
class TransformStream::DefaultSourceCancelAlgorithm final
    : public StreamAlgorithm {
 public:
  explicit DefaultSourceCancelAlgorithm(TransformStream* stream)
      : stream_(stream) {}

  v8::Local<v8::Promise> Run(v8::Isolate* isolate,
                             base::span<v8::Local<v8::Value>> argv) override {
    CHECK_EQ(argv.size(), 1u);

    // https://streams.spec.whatwg.org/#initialize-transform-stream
    // 7. Let cancelAlgorithm be the following steps, taking a reason argument:
    //    a. Perform ! TransformStreamErrorWritableAndUnblockWrite(stream,
    //       reason).
    ErrorWritableAndUnblockWrite(isolate, stream_, argv[0]);

    //    b. Return a promise resolved with undefined.
    return bindings::ResolvedUndefinedPromise(isolate);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(stream_);
    StreamAlgorithm::Trace(visitor);
  }

 private:
  cppgc::Member<TransformStream> stream_;
};

// Split out from the constructor, as calling JavaScript from inside a C++
// constructor can cause GC problems.
void TransformStream::InitInternal(v8::Isolate* isolate,
                                   v8::Local<v8::Value> raw_transformer,
                                   v8::Local<v8::Value> raw_writable_strategy,
                                   v8::Local<v8::Value> raw_readable_strategy,
                                   bindings::ExceptionState& exception_state) {
  CHECK(!raw_transformer.IsEmpty());
  CHECK(!raw_writable_strategy.IsEmpty());
  CHECK(!raw_readable_strategy.IsEmpty());

  const v8::Local<v8::Context> context = isolate->GetCurrentContext();

  // https://streams.spec.whatwg.org/#ts-constructor
  // Perform the "transformer = {}" step from the function signature.
  v8::Local<v8::Object> transformer;
  ScriptValueToObject(isolate, raw_transformer, &transformer, exception_state);
  if (exception_state.HadException()) {
    return;
  }

  // Perform the "writableStrategy = {}" step from the function signature, and
  // 1. Let writableSizeFunction be ? GetV(writableStrategy, "size").
  // 2. Let writableHighWaterMark be ? GetV(writableStrategy, "highWaterMark").
  StrategyUnpacker writable_strategy_unpacker(isolate, raw_writable_strategy,
                                              exception_state);
  if (exception_state.HadException()) {
    return;
  }

  // Perform the "readableStrategy = {}" step from the function signature, and
  // 3. Let readableSizeFunction be ? GetV(readableStrategy, "size").
  // 4. Let readableHighWaterMark be ? GetV(readableStrategy, "highWaterMark").
  StrategyUnpacker readable_strategy_unpacker(isolate, raw_readable_strategy,
                                              exception_state);
  if (exception_state.HadException()) {
    return;
  }

  // 5. Let writableType be ? GetV(transformer, "writableType").
  v8::Local<v8::Value> writable_type;
  {
    const bindings::TryRethrowScope rethrow_scope(isolate, exception_state);
    if (!transformer
             ->Get(context, v8::String::NewFromUtf8Literal(
                                isolate, "writableType",
                                v8::NewStringType::kInternalized))
             .ToLocal(&writable_type)) {
      return;
    }
  }

  // 6. If writableType is not undefined, throw a RangeError exception.
  if (!writable_type->IsUndefined()) {
    exception_state.ThrowRangeError("Invalid writableType was specified");
    return;
  }

  // 7. Let writableSizeAlgorithm be ? MakeSizeAlgorithmFromSizeFunction(
  //    writableSizeFunction).
  StrategySizeAlgorithm* const writable_size_algorithm =
      writable_strategy_unpacker.MakeSizeAlgorithm(isolate, exception_state);
  if (exception_state.HadException()) {
    return;
  }

  // 8. If writableHighWaterMark is undefined, set writableHighWaterMark to 1.
  // 9. Set writableHighWaterMark to ? ValidateAndNormalizeHighWaterMark(
  //    writableHighWaterMark).
  const double writable_high_water_mark =
      writable_strategy_unpacker.GetHighWaterMark(isolate, 1, exception_state);
  if (exception_state.HadException()) {
    return;
  }

  // 10. Let readableType be ? GetV(transformer, "readableType").
  v8::Local<v8::Value> readable_type;
  {
    const bindings::TryRethrowScope rethrow_scope(isolate, exception_state);
    if (!transformer
             ->Get(context, v8::String::NewFromUtf8Literal(
                                isolate, "readableType",
                                v8::NewStringType::kInternalized))
             .ToLocal(&readable_type)) {
      return;
    }
  }

  // 11. If readableType is not undefined, throw a RangeError exception.
  if (!readable_type->IsUndefined()) {
    exception_state.ThrowRangeError("Invalid readableType was specified");
    return;
  }

  // 12. Let readableSizeAlgorithm be ? MakeSizeAlgorithmFromSizeFunction(
  //     readableSizeFunction).
  StrategySizeAlgorithm* const readable_size_algorithm =
      readable_strategy_unpacker.MakeSizeAlgorithm(isolate, exception_state);
  if (exception_state.HadException()) {
    return;
  }

  // 13. If readableHighWaterMark is undefined, set readableHighWaterMark to 0.
  // 14. Set readableHighWaterMark be ? ValidateAndNormalizeHighWaterMark(
  //     readableHighWaterMark).
  const double readable_high_water_mark =
      readable_strategy_unpacker.GetHighWaterMark(isolate, 0, exception_state);
  if (exception_state.HadException()) {
    return;
  }

  // 15. Let startPromise be a new promise.
  auto* const start_promise =
      bindings::MakeGarbageCollected<bindings::ScriptPromiseResolver>(isolate);

  // 16. Perform ! InitializeTransformStream(this, startPromise,
  //     writableHighWaterMark, writableSizeAlgorithm, readableHighWaterMark,
  //     readableSizeAlgorithm).
  Initialize(isolate, this, start_promise, writable_high_water_mark,
             writable_size_algorithm, readable_high_water_mark,
             readable_size_algorithm, exception_state);
  if (exception_state.HadException()) {
    return;
  }

  // 17. Perform ? SetUpTransformStreamDefaultControllerFromTransformer(this,
  //     transformer).
  const v8::Local<v8::Value> controller_value =
      TransformStreamDefaultController::SetUpFromTransformer(
          isolate, this, transformer, exception_state);
  if (exception_state.HadException()) {
    return;
  }

  // 18. Let startResult be ? InvokeOrNoop(transformer, "start", « this.
  //     [[transformStreamController]] »).
  const v8::MaybeLocal<v8::Value> start_result_maybe =
      CallOrNoop1(isolate, transformer, "start", "transformer.start",
                  controller_value, exception_state);
  v8::Local<v8::Value> start_result;
  if (!start_result_maybe.ToLocal(&start_result)) {
    CHECK(exception_state.HadException());
    return;
  }
  CHECK(!exception_state.HadException());

  // 19. Resolve startPromise with startResult.
  start_promise->Resolve(isolate,
                         bindings::PromiseResolve(isolate, start_result));
}

// static
void TransformStream::Initialize(v8::Isolate* isolate,
                                 TransformStream* stream,
                                 bindings::ScriptPromiseResolver* start_promise,
                                 double writable_high_water_mark,
                                 StrategySizeAlgorithm* writable_size_algorithm,
                                 double readable_high_water_mark,
                                 StrategySizeAlgorithm* readable_size_algorithm,
                                 bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#initialize-transform-stream
  // 1. Let startAlgorithm be an algorithm that returns startPromise.
  auto* const start_algorithm =
      bindings::MakeGarbageCollected<ReturnStartPromiseAlgorithm>(
          start_promise);

  // 2. Let writeAlgorithm be the following steps, taking a chunk argument:
  //    a. Return ! TransformStreamDefaultSinkWriteAlgorithm(stream, chunk).
  auto* const write_algorithm =
      bindings::MakeGarbageCollected<DefaultSinkWriteAlgorithm>(stream);

  // 3. Let abortAlgorithm be the following steps, taking a reason argument:
  //    a. Return ! TransformStreamDefaultSinkAbortAlgorithm(stream, reason).
  auto* const abort_algorithm =
      bindings::MakeGarbageCollected<DefaultSinkAbortAlgorithm>(stream);

  // 4. Let closeAlgorithm be the following steps:
  //    a. Return ! TransformStreamDefaultSinkCloseAlgorithm(stream).
  auto* const close_algorithm =
      bindings::MakeGarbageCollected<DefaultSinkCloseAlgorithm>(stream);

  // 5. Set stream.[[writable]] to ! CreateWritableStream(startAlgorithm,
  //    writeAlgorithm, closeAlgorithm, abortAlgorithm, writableHighWaterMark,
  //    writableSizeAlgorithm).
  stream->writable_ = WritableStream::Create(
      isolate, start_algorithm, write_algorithm, close_algorithm,
      abort_algorithm, writable_high_water_mark, writable_size_algorithm,
      exception_state);
  if (exception_state.HadException()) {
    return;
  }

  CHECK(stream->writable_);

  // 6. Let pullAlgorithm be the following steps:
  //    a. Return ! TransformStreamDefaultSourcePullAlgorithm(stream).
  auto* const pull_algorithm =
      bindings::MakeGarbageCollected<DefaultSourcePullAlgorithm>(stream);

  // 7. Let cancelAlgorithm be the following steps, taking a reason argument:
  //    a. Perform ! TransformStreamErrorWritableAndUnblockWrite(stream,
  //       reason).
  //    b. Return a promise resolved with undefined.
  auto* const cancel_algorithm =
      bindings::MakeGarbageCollected<DefaultSourceCancelAlgorithm>(stream);

  // 8. Set stream.[[readable]] to ! CreateReadableStream(startAlgorithm,
  //    pullAlgorithm, cancelAlgorithm, readableHighWaterMark,
  //    readableSizeAlgorithm).
  stream->readable_ = ReadableStream::Create(
      isolate, start_algorithm, pull_algorithm, cancel_algorithm,
      readable_high_water_mark, readable_size_algorithm, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  CHECK(stream->readable_);

  //  9. Set stream.[[backpressure]] and stream.[[backpressureChangePromise]] to
  //     undefined.
  // 10. Perform ! TransformStreamSetBackpressure(stream, true).
  // |had_backpressure_| is bool and so can't be set to undefined; instead we
  // take the equivalent steps to achieve the final result here.
  CHECK(stream->had_backpressure_);
  CHECK(!stream->backpressure_change_promise_);
  stream->backpressure_change_promise_ =
      bindings::MakeGarbageCollected<bindings::ScriptPromiseResolver>(isolate);

  // 11. Set stream.[[transformStreamController]] to undefined.
  // (This is set by the constructor; just verify the value here).
  CHECK(!stream->transform_stream_controller_);
}

// static
void TransformStream::Error(v8::Isolate* isolate,
                            TransformStream* stream,
                            v8::Local<v8::Value> e) {
  // https://streams.spec.whatwg.org/#transform-stream-error
  // 1. Perform ! ReadableStreamDefaultControllerError(stream.[[readable]].
  //    [[readableStreamController]], e).
  ReadableStreamDefaultController::Error(isolate,
                                         stream->GetReadableController(), e);

  // 2. Perform ! TransformStreamErrorWritableAndUnblockWrite(stream, e).
  ErrorWritableAndUnblockWrite(isolate, stream, e);
}

// static
void TransformStream::ErrorWritableAndUnblockWrite(v8::Isolate* isolate,
                                                   TransformStream* stream,
                                                   v8::Local<v8::Value> e) {
  // https://streams.spec.whatwg.org/#transform-stream-error-writable-and-unblock-write
  // 1. Perform ! TransformStreamDefaultControllerClearAlgorithms(stream.
  //    [[transformStreamController]]).
  TransformStreamDefaultController::ClearAlgorithms(
      stream->transform_stream_controller_);

  // 2. Perform !
  //    WritableStreamDefaultControllerErrorIfNeeded(stream.[[writable]].
  //    [[writableStreamController]], e).
  WritableStreamDefaultController::ErrorIfNeeded(
      isolate, stream->writable_->Controller(), e);

  // 3. If stream.[[backpressure]] is true, perform !
  //    TransformStreamSetBackpressure(stream, false).
  if (stream->had_backpressure_) {
    SetBackpressure(isolate, stream, false);
  }
}

// static
void TransformStream::SetBackpressure(v8::Isolate* isolate,
                                      TransformStream* stream,
                                      bool backpressure) {
  // https://streams.spec.whatwg.org/#transform-stream-set-backpressure
  // 1. Assert: stream.[[backpressure]] is not backpressure.
  CHECK_NE(stream->had_backpressure_, backpressure);

  // 2. If stream.[[backpressureChangePromise]] is not undefined, resolve
  //    stream.[[backpressureChangePromise]] with undefined.
  // In the standard, [[backpressureChangePromise]] is initialized by calling
  // this function. However, in this implementation it is initialized in
  // InitializeTransformStream() without calling this function. As a result,
  // the function is never called without |backpressure_change_promise_| set
  // and we don't need to test it.
  CHECK(stream->backpressure_change_promise_);
  stream->backpressure_change_promise_->Resolve(isolate);

  // 3. Set stream.[[backpressureChangePromise]] to a new promise.
  stream->backpressure_change_promise_ =
      bindings::MakeGarbageCollected<bindings::ScriptPromiseResolver>(isolate);

  // 4. Set stream.[[backpressure]] to backpressure.
  stream->had_backpressure_ = backpressure;
}

}  // namespace mitto
