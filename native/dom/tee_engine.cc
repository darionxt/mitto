#include "mitto/dom/tee_engine.h"

#include <array>
#include <optional>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/functional/bind.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_function.h"
#include "mitto/bindings/script_promise.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/miscellaneous_operations.h"
#include "mitto/dom/read_request.h"
#include "mitto/dom/readable_stream.h"
#include "mitto/dom/readable_stream_default_controller.h"
#include "mitto/dom/readable_stream_default_reader.h"
#include "mitto/dom/serialized_script_value.h"
#include "mitto/dom/stream_algorithms.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

v8::MaybeLocal<v8::Value> TeeEngine::StructuredClone(
    v8::Isolate* isolate,
    v8::Local<v8::Value> chunk,
    bindings::ExceptionState& exception_state) {
  // Step 1. Let serialized be ? StructuredSerialize(v).
  std::optional<SerializedScriptValue> serialized =
      SerializedScriptValue::Serialize(isolate, chunk);
  if (!serialized) {
    // Blink throws a DataCloneError of its own in place of the serializer's.
    exception_state.ThrowDOMException(
        "DataCloneError", "chunk could not be cloned.");
    return {};
  }
  // Step 2. Return ? StructuredDeserialize(serialized, the current Realm).
  return serialized->Deserialize(isolate);
}

class TeeEngine::TeeReadRequest final : public ReadRequest {
 public:
  explicit TeeReadRequest(TeeEngine* engine) : engine_(engine) {}

  void ChunkSteps(v8::Isolate* isolate,
                  v8::Local<v8::Value> chunk,
                  bindings::ExceptionState&) const override {
    // The chunk steps run in a microtask of their own, as the standard says.
    EnqueueMicrotask(
        isolate,
        base::BindOnce(
            [](cppgc::Persistent<const TeeReadRequest> self,
               v8::Global<v8::Value> value) {
              self->ChunkStepsBody(std::move(value));
            },
            cppgc::Persistent<const TeeReadRequest>(this),
            v8::Global<v8::Value>(isolate, chunk)));
  }

  void CloseSteps(v8::Isolate* isolate) const override {
    // Step 1. Set reading to false.
    engine_->reading_ = false;
    // Steps 2-3. If canceled1 is false, close branch1's controller, and the
    // same for branch2.
    for (int branch = 0; branch < 2; ++branch) {
      if (!engine_->canceled_[branch] &&
          ReadableStreamDefaultController::CanCloseOrEnqueue(
              engine_->controller_[branch])) {
        ReadableStreamDefaultController::Close(isolate,
                                               engine_->controller_[branch]);
      }
    }
    // Step 4. If canceled1 is false or canceled2 is false, resolve
    // cancelPromise with undefined.
    if (!engine_->canceled_[0] || !engine_->canceled_[1]) {
      engine_->cancel_promise_->Resolve(isolate);
    }
  }

  void ErrorSteps(v8::Isolate*, v8::Local<v8::Value>) const override {
    // Step 1. Set reading to false.
    engine_->reading_ = false;
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(engine_);
    ReadRequest::Trace(visitor);
  }

 private:
  void ChunkStepsBody(v8::Global<v8::Value> value) const;

  const cppgc::Member<TeeEngine> engine_;
};

class TeeEngine::PullAlgorithm final : public StreamAlgorithm {
 public:
  explicit PullAlgorithm(TeeEngine* engine) : engine_(engine) {}

  v8::Local<v8::Promise> Run(v8::Isolate* isolate,
                             base::span<v8::Local<v8::Value>>) override {
    // Step 13. Let pullAlgorithm be the following steps. a. If reading is
    // true, set readAgain to true and return a promise resolved with
    // undefined.
    if (engine_->reading_) {
      engine_->read_again_ = true;
      return bindings::ResolvedUndefinedPromise(isolate);
    }
    // b. Set reading to true.
    engine_->reading_ = true;
    // c-d. Read from the reader with a read request of the tee.
    auto* const read_request =
        bindings::MakeGarbageCollected<TeeReadRequest>(engine_);
    bindings::ExceptionState pass_through(isolate);
    ReadableStreamDefaultReader::Read(isolate, engine_->reader_, read_request,
                                      pass_through);
    // e. Return a promise resolved with undefined.
    return bindings::ResolvedUndefinedPromise(isolate);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(engine_);
    StreamAlgorithm::Trace(visitor);
  }

 private:
  const cppgc::Member<TeeEngine> engine_;
};

void TeeEngine::TeeReadRequest::ChunkStepsBody(
    v8::Global<v8::Value> value) const {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::TryCatch try_catch(isolate);
  // Step 1. Set readAgain to false.
  engine_->read_again_ = false;
  // Step 2. Let chunk1 and chunk2 be chunk.
  std::array<v8::Local<v8::Value>, 2> chunk;
  chunk[0] = value.Get(isolate);
  chunk[1] = chunk[0];
  // Step 3. If canceled2 is false and cloneForBranch2 is true:
  if (!engine_->canceled_[1] && engine_->clone_for_branch2_) {
    // a. Let cloneResult be StructuredClone(chunk2).
    bindings::ExceptionState pass_through(isolate);
    v8::Local<v8::Value> clone_result;
    if (!engine_->StructuredClone(isolate, chunk[1], pass_through)
             .ToLocal(&clone_result)) {
      // b. If cloneResult is an abrupt completion: error both branches with
      // it, resolve cancelPromise with ! ReadableStreamCancel(stream,
      // cloneResult.[[Value]]), and return.
      CHECK(try_catch.HasCaught());
      const v8::Local<v8::Value> exception = try_catch.Exception();
      ReadableStreamDefaultController::Error(isolate, engine_->controller_[0],
                                             exception);
      ReadableStreamDefaultController::Error(isolate, engine_->controller_[1],
                                             exception);
      engine_->cancel_promise_->Resolve(
          isolate, ReadableStream::Cancel(isolate, engine_->stream_, exception));
      return;
    }
    // c. Otherwise, set chunk2 to cloneResult.[[Value]].
    chunk[1] = clone_result;
  }
  // Steps 4-5. If canceled1 is false, enqueue chunk1 in branch1's controller,
  // and the same for branch2.
  for (int branch = 0; branch < 2; ++branch) {
    if (!engine_->canceled_[branch] &&
        ReadableStreamDefaultController::CanCloseOrEnqueue(
            engine_->controller_[branch])) {
      bindings::ExceptionState pass_through(isolate);
      ReadableStreamDefaultController::Enqueue(
          isolate, engine_->controller_[branch], chunk[branch], pass_through);
      if (try_catch.HasCaught()) {
        // Blink errors the controller at once instead of returning a
        // rejection, which nothing can tell apart.
        ReadableStreamDefaultController::Error(
            isolate, engine_->controller_[branch], try_catch.Exception());
        return;
      }
    }
  }
  // Step 6. Set reading to false.
  engine_->reading_ = false;
  // Step 7. If readAgain is true, perform pullAlgorithm.
  if (engine_->read_again_) {
    bindings::MakeGarbageCollected<PullAlgorithm>(engine_)->Run(isolate, {});
  }
}

class TeeEngine::CancelAlgorithm final : public StreamAlgorithm {
 public:
  CancelAlgorithm(TeeEngine* engine, int branch)
      : engine_(engine), branch_(branch) {
    CHECK(branch == 0 || branch == 1);
  }

  v8::Local<v8::Promise> Run(
      v8::Isolate* isolate,
      base::span<v8::Local<v8::Value>> arguments) override {
    // cancel1Algorithm and cancel2Algorithm, which differ only in their
    // branch. a. Set canceled1 to true.
    engine_->canceled_[branch_] = true;
    CHECK_EQ(arguments.size(), 1u);
    // b. Set reason1 to reason.
    engine_->reason_[branch_].Reset(isolate, arguments[0]);
    const int other_branch = 1 - branch_;
    // c. If canceled2 is true:
    if (engine_->canceled_[other_branch]) {
      // i. Let compositeReason be ! CreateArrayFromList(« reason1, reason2 »).
      v8::Local<v8::Value> reason[] = {engine_->reason_[0].Get(isolate),
                                       engine_->reason_[1].Get(isolate)};
      const v8::Local<v8::Value> composite_reason =
          v8::Array::New(isolate, reason, 2);
      // ii-iii. Resolve cancelPromise with ! ReadableStreamCancel(stream,
      // compositeReason).
      engine_->cancel_promise_->Resolve(
          isolate,
          ReadableStream::Cancel(isolate, engine_->stream_, composite_reason));
    }
    // d. Return cancelPromise.
    return engine_->cancel_promise_->Promise(isolate);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(engine_);
    StreamAlgorithm::Trace(visitor);
  }

 private:
  const cppgc::Member<TeeEngine> engine_;
  const int branch_;
};

TeeEngine::TeeEngine() = default;

TeeEngine::~TeeEngine() = default;

void TeeEngine::Start(v8::Isolate* isolate,
                      ReadableStream* stream,
                      bool clone_for_branch2,
                      bindings::ExceptionState& exception_state) {
  // Step 1. Assert: stream implements ReadableStream.
  CHECK(stream);
  stream_ = stream;
  // Step 2. Assert: cloneForBranch2 is a boolean.
  clone_for_branch2_ = clone_for_branch2;
  // Step 3. Let reader be ? AcquireReadableStreamDefaultReader(stream).
  reader_ =
      ReadableStream::AcquireDefaultReader(isolate, stream, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  // Steps 4-11 are the fields' initial values. Step 12. Let cancelPromise be
  // a new promise.
  cancel_promise_ =
      bindings::MakeGarbageCollected<bindings::ScriptPromiseResolver>(isolate);
  // Steps 13-16. The pull, cancel and start algorithms.
  auto* const pull_algorithm =
      bindings::MakeGarbageCollected<PullAlgorithm>(this);
  auto* const cancel1_algorithm =
      bindings::MakeGarbageCollected<CancelAlgorithm>(this, 0);
  auto* const cancel2_algorithm =
      bindings::MakeGarbageCollected<CancelAlgorithm>(this, 1);
  StreamStartAlgorithm* const start_algorithm = CreateTrivialStartAlgorithm();
  StrategySizeAlgorithm* const size_algorithm = CreateDefaultSizeAlgorithm();
  // Step 17. Set branch1 to ! CreateReadableStream(startAlgorithm,
  // pullAlgorithm, cancel1Algorithm).
  branch_[0] = ReadableStream::Create(isolate, start_algorithm, pull_algorithm,
                                      cancel1_algorithm, 1.0, size_algorithm,
                                      exception_state);
  if (exception_state.HadException()) {
    return;
  }
  // Step 18. Set branch2 to ! CreateReadableStream(startAlgorithm,
  // pullAlgorithm, cancel2Algorithm).
  branch_[1] = ReadableStream::Create(isolate, start_algorithm, pull_algorithm,
                                      cancel2_algorithm, 1.0, size_algorithm,
                                      exception_state);
  if (exception_state.HadException()) {
    return;
  }
  for (int branch = 0; branch < 2; ++branch) {
    // The branches were just made with default controllers.
    controller_[branch] = static_cast<ReadableStreamDefaultController*>(
        branch_[branch]->GetController());
  }

  class RejectFunction final : public bindings::ScriptFunction {
   public:
    explicit RejectFunction(TeeEngine* engine) : engine_(engine) {}

    v8::Local<v8::Value> Call(v8::Isolate* isolate,
                              v8::Local<v8::Value> r) override {
      // Step 19. Upon rejection of reader.[[closedPromise]] with reason r:
      // error both branches with r, and if canceled1 is false or canceled2
      // is false, resolve cancelPromise with undefined.
      ReadableStreamDefaultController::Error(isolate, engine_->controller_[0],
                                             r);
      ReadableStreamDefaultController::Error(isolate, engine_->controller_[1],
                                             r);
      if (!engine_->canceled_[0] || !engine_->canceled_[1]) {
        engine_->cancel_promise_->Resolve(isolate);
      }
      return v8::Undefined(isolate);
    }

    void Trace(cppgc::Visitor* visitor) const override {
      visitor->Trace(engine_);
      bindings::ScriptFunction::Trace(visitor);
    }

   private:
    const cppgc::Member<TeeEngine> engine_;
  };

  bindings::PromiseThen(isolate, reader_->Closed(), nullptr,
                        bindings::MakeGarbageCollected<RejectFunction>(this));
  // Step 20, returning the branches, is the caller's.
}

void TeeEngine::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(stream_);
  visitor->Trace(reader_);
  visitor->Trace(reason_[0]);
  visitor->Trace(reason_[1]);
  visitor->Trace(branch_[0]);
  visitor->Trace(branch_[1]);
  visitor->Trace(controller_[0]);
  visitor->Trace(controller_[1]);
  visitor->Trace(cancel_promise_);
}

}  // namespace mitto
