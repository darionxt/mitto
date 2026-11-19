#include "mitto/dom/byte_stream_tee_engine.h"

#include <array>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_function.h"
#include "mitto/bindings/script_promise.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/miscellaneous_operations.h"
#include "mitto/dom/read_request.h"
#include "mitto/dom/readable_byte_stream_controller.h"
#include "mitto/dom/readable_stream.h"
#include "mitto/dom/readable_stream_byob_reader.h"
#include "mitto/dom/readable_stream_byob_request.h"
#include "mitto/dom/readable_stream_controller.h"
#include "mitto/dom/readable_stream_default_reader.h"
#include "mitto/dom/readable_stream_generic_reader.h"
#include "mitto/dom/stream_algorithms.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-cppgc.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

namespace {

// The byte stream controller of a branch, which the tee made a byte stream.
ReadableByteStreamController* ByteControllerOf(
    ReadableStreamController* controller) {
  CHECK(controller->IsByteStreamController());
  return static_cast<ReadableByteStreamController*>(controller);
}

}  // namespace

class ByteStreamTeeEngine::PullAlgorithm final : public StreamAlgorithm {
 public:
  PullAlgorithm(ByteStreamTeeEngine* engine, int branch)
      : engine_(engine), branch_(branch) {
    CHECK(branch == 0 || branch == 1);
  }

  v8::Local<v8::Promise> Run(v8::Isolate* isolate,
                             base::span<v8::Local<v8::Value>>) override {
    // https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamtee
    // This implements both pull1Algorithm and pull2Algorithm as they are
    // identical except for the index they operate on. Standard comments are
    // from pull1Algorithm.
    // 17. Let pull1Algorithm be the following steps:
    //   a. If reading is true,
    bindings::ExceptionState exception_state(isolate);
    if (engine_->reading_) {
      //     i. Set readAgainForBranch1 to true.
      engine_->read_again_for_branch_[branch_] = true;
      //     ii. Return a promise resolved with undefined.
      return bindings::ResolvedUndefinedPromise(isolate);
    }
    //   b. Set reading to true.
    engine_->reading_ = true;
    //   c. Let byobRequest be !
    //   ReadableByteStreamControllerGetBYOBRequest(branch1.[[controller]]).
    ReadableStreamBYOBRequest* byob_request =
        ReadableByteStreamController::GetBYOBRequest(
            engine_->controller_[branch_]);
    //   d. If byobRequest is null, perform pullWithDefaultReader.
    if (!byob_request) {
      engine_->PullWithDefaultReader(isolate, exception_state);
    } else {
      //   e. Otherwise, perform pullWithBYOBReader, given byobRequest.[[view]]
      //   and false.
      engine_->PullWithBYOBReader(isolate, byob_request->View(), branch_ == 1,
                                  exception_state);
    }
    //   f. Return a promise resolved with undefined.
    return bindings::ResolvedUndefinedPromise(isolate);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(engine_);
    StreamAlgorithm::Trace(visitor);
  }

 private:
  const cppgc::Member<ByteStreamTeeEngine> engine_;
  const int branch_;
};

class ByteStreamTeeEngine::CancelAlgorithm final : public StreamAlgorithm {
 public:
  CancelAlgorithm(ByteStreamTeeEngine* engine, int branch)
      : engine_(engine), branch_(branch) {
    CHECK(branch == 0 || branch == 1);
  }

  v8::Local<v8::Promise> Run(v8::Isolate* isolate,
                             base::span<v8::Local<v8::Value>> argv) override {
    // https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamtee
    // This implements both cancel1Algorithm and cancel2Algorithm as they are
    // identical except for the index they operate on. Standard comments are
    // from cancel1Algorithm.
    // 19. Let cancel1Algorithm be the following steps, taking a reason
    // argument:
    //   a. Set canceled1 to true.
    engine_->canceled_[branch_] = true;
    //   b. Set reason1 to reason.
    CHECK_EQ(argv.size(), 1u);
    engine_->reason_[branch_].Reset(isolate, argv[0]);
    //   c. If canceled2 is true,
    const int other_branch = 1 - branch_;
    if (engine_->canceled_[other_branch]) {
      //     i. Let compositeReason be ! CreateArrayFromList(« reason1, reason2
      //     »).
      v8::Local<v8::Value> reason[] = {engine_->reason_[0].Get(isolate),
                                       engine_->reason_[1].Get(isolate)};
      const v8::Local<v8::Value> composite_reason =
          v8::Array::New(isolate, reason, 2);
      //     ii. Let cancelResult be ! ReadableStreamCancel(stream,
      //     compositeReason).
      const v8::Local<v8::Promise> cancel_result = ReadableStream::Cancel(
          isolate, engine_->stream_.Get(), composite_reason);
      //     iii. Resolve cancelPromise with cancelResult.
      engine_->cancel_promise_->Resolve(isolate, cancel_result);
    }
    //   d. Return cancelPromise.
    return engine_->cancel_promise_->Promise(isolate);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(engine_);
    StreamAlgorithm::Trace(visitor);
  }

 private:
  const cppgc::Member<ByteStreamTeeEngine> engine_;
  const int branch_;
};

class ByteStreamTeeEngine::ByteTeeReadRequest final : public ReadRequest {
 public:
  explicit ByteTeeReadRequest(ByteStreamTeeEngine* engine) : engine_(engine) {}

  void ChunkSteps(v8::Isolate* isolate,
                  v8::Local<v8::Value> chunk,
                  bindings::ExceptionState&) const override {
    // The chunk steps run in a microtask of their own, as the standard says.
    EnqueueMicrotask(isolate,
                     base::BindOnce(
                         [](cppgc::Persistent<const ByteTeeReadRequest> self,
                            v8::Global<v8::Value> value) {
                           self->ChunkStepsBody(std::move(value));
                         },
                         cppgc::Persistent<const ByteTeeReadRequest>(this),
                         v8::Global<v8::Value>(isolate, chunk)));
  }

  void CloseSteps(v8::Isolate* isolate) const override {
    // 1. Set reading to false.
    engine_->reading_ = false;
    const v8::TryCatch try_catch(isolate);
    // 2. If canceled1 is false, perform !
    // ReadableByteStreamControllerClose(branch1.[[controller]]).
    // 3. If canceled2 is false, perform !
    // ReadableByteStreamControllerClose(branch2.[[controller]]).
    for (int branch = 0; branch < 2; ++branch) {
      if (!engine_->canceled_[branch]) {
        ReadableByteStreamController::Close(isolate,
                                            engine_->controller_[branch]);
        if (try_catch.HasCaught()) {
          // Instead of returning a rejection, which is inconvenient here,
          // call ControllerError(). The only difference this makes is that it
          // happens synchronously, but that should not be observable.
          ReadableByteStreamController::Error(
              isolate, engine_->controller_[branch], try_catch.Exception());
          return;
        }
      }
    }
    // 4. If branch1.[[controller]].[[pendingPullIntos]] is not empty, perform
    // ! ReadableByteStreamControllerRespond(branch1.[[controller]], 0).
    // 5. If branch2.[[controller]].[[pendingPullIntos]] is not empty, perform
    // ! ReadableByteStreamControllerRespond(branch2.[[controller]], 0).
    for (int branch = 0; branch < 2; ++branch) {
      if (!engine_->controller_[branch]->pending_pull_intos_.empty()) {
        bindings::ExceptionState pass_through(isolate);
        ReadableByteStreamController::Respond(
            isolate, engine_->controller_[branch], 0, pass_through);
        if (try_catch.HasCaught()) {
          // Instead of returning a rejection, which is inconvenient here,
          // call ControllerError(). The only difference this makes is that it
          // happens synchronously, but that should not be observable.
          ReadableByteStreamController::Error(
              isolate, engine_->controller_[branch], try_catch.Exception());
          return;
        }
      }
    }
    // 6. If canceled1 is false or canceled2 is false, resolve cancelPromise
    // with undefined.
    if (!engine_->canceled_[0] || !engine_->canceled_[1]) {
      engine_->cancel_promise_->Resolve(isolate);
    }
  }

  void ErrorSteps(v8::Isolate*, v8::Local<v8::Value>) const override {
    // 1. Set reading to false.
    engine_->reading_ = false;
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(engine_);
    ReadRequest::Trace(visitor);
  }

 private:
  void ChunkStepsBody(v8::Global<v8::Value> value) const {
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    // 1. Set readAgainForBranch1 to false.
    engine_->read_again_for_branch_[0] = false;
    // 2. Set readAgainForBranch2 to false.
    engine_->read_again_for_branch_[1] = false;

    // 3. Let chunk1 and chunk2 be chunk.
    // The chunks of a byte stream's default reader are Uint8Arrays.
    const v8::Local<v8::Value> chunk_value = value.Get(isolate);
    CHECK(chunk_value->IsUint8Array());
    const v8::Local<v8::Uint8Array> buffer_view =
        chunk_value.As<v8::Uint8Array>();
    std::array<v8::Local<v8::Uint8Array>, 2> chunk = {buffer_view, buffer_view};

    // 4. If canceled1 is false and canceled2 is false,
    if (!engine_->canceled_[0] && !engine_->canceled_[1]) {
      //   a. Let cloneResult be CloneAsUint8Array(chunk).
      const v8::Local<v8::Uint8Array> clone_result =
          engine_->CloneAsUint8Array(isolate, buffer_view);
      //   b. If cloneResult is an abrupt completion,
      //     i. Perform !
      //     ReadableByteStreamControllerError(branch1.[[controller]],
      //     cloneResult.[[Value]]).
      //     ii. Perform !
      //     ReadableByteStreamControllerError(branch2.[[controller]],
      //     cloneResult.[[Value]]).
      //     iii. Resolve cancelPromise with !
      //     ReadableStreamCancel(stream, cloneResult.[[Value]]).
      //     iv. Return.
      //   This is not needed as v8::ArrayBuffer::New(), which is used in
      //   CloneAsUint8Array(), crashes if it cannot allocate the memory.

      //   c. Otherwise, set chunk2 to cloneResult.[[Value]].
      chunk[1] = clone_result;
    }

    // 5. If canceled1 is false, perform !
    // ReadableByteStreamControllerEnqueue(branch1.[[controller]], chunk1).
    // 6. If canceled2 is false, perform !
    // ReadableByteStreamControllerEnqueue(branch2.[[controller]], chunk2).
    for (int branch = 0; branch < 2; ++branch) {
      if (!engine_->canceled_[branch]) {
        const v8::TryCatch try_catch(isolate);
        bindings::ExceptionState pass_through(isolate);
        ReadableByteStreamController::Enqueue(
            isolate, engine_->controller_[branch], chunk[branch], pass_through);
        if (try_catch.HasCaught()) {
          // Instead of returning a rejection, which is inconvenient here,
          // call ControllerError(). The only difference this makes is that it
          // happens synchronously, but that should not be observable.
          ReadableByteStreamController::Error(
              isolate, engine_->controller_[branch], try_catch.Exception());
          return;
        }
      }
    }

    // 7. Set reading to false.
    engine_->reading_ = false;

    // 8. If readAgainForBranch1 is true, perform pull1Algorithm.
    if (engine_->read_again_for_branch_[0]) {
      auto* const pull_algorithm =
          bindings::MakeGarbageCollected<PullAlgorithm>(engine_.Get(), 0);
      pull_algorithm->Run(isolate, {});
      // 9. Otherwise, if readAgainForBranch2 is true, perform pull2Algorithm.
    } else if (engine_->read_again_for_branch_[1]) {
      auto* const pull_algorithm =
          bindings::MakeGarbageCollected<PullAlgorithm>(engine_.Get(), 1);
      pull_algorithm->Run(isolate, {});
    }
  }

  const cppgc::Member<ByteStreamTeeEngine> engine_;
};

class ByteStreamTeeEngine::ByteTeeReadIntoRequest final
    : public ReadIntoRequest {
 public:
  ByteTeeReadIntoRequest(ByteStreamTeeEngine* engine,
                         ReadableStream* byob_branch,
                         ReadableStream* other_branch,
                         bool for_branch_2)
      : engine_(engine),
        byob_branch_(byob_branch),
        other_branch_(other_branch),
        for_branch_2_(for_branch_2) {}

  void ChunkSteps(v8::Isolate* isolate,
                  v8::Local<v8::ArrayBufferView> chunk,
                  bindings::ExceptionState&) const override {
    // The chunk steps run in a microtask of their own, as the standard says.
    EnqueueMicrotask(
        isolate, base::BindOnce(
                     [](cppgc::Persistent<const ByteTeeReadIntoRequest> self,
                        v8::Global<v8::ArrayBufferView> chunk_global) {
                       self->ChunkStepsBody(std::move(chunk_global));
                     },
                     cppgc::Persistent<const ByteTeeReadIntoRequest>(this),
                     v8::Global<v8::ArrayBufferView>(isolate, chunk)));
  }

  void CloseSteps(v8::Isolate* isolate,
                  v8::Local<v8::ArrayBufferView> chunk) const override {
    // 1. Set reading to false.
    engine_->reading_ = false;
    // 2. Let byobCanceled be canceled2 if forBranch2 is true, and canceled1
    //    otherwise.
    const bool byob_canceled =
        for_branch_2_ ? engine_->canceled_[1] : engine_->canceled_[0];
    // 3. Let otherCanceled be canceled2 if forBranch2 is false, and canceled1
    //    otherwise.
    const bool other_canceled =
        !for_branch_2_ ? engine_->canceled_[1] : engine_->canceled_[0];
    // 4. If byobCanceled is false, perform !
    //    ReadableByteStreamControllerClose(byobBranch.[[controller]]).
    if (!byob_canceled) {
      ReadableByteStreamController::Close(
          isolate, ByteControllerOf(byob_branch_->readable_stream_controller_));
    }
    // 5. If otherCanceled is false, perform !
    //    ReadableByteStreamControllerClose(otherBranch.[[controller]]).
    if (!other_canceled) {
      ReadableByteStreamController::Close(
          isolate,
          ByteControllerOf(other_branch_->readable_stream_controller_));
    }
    // 6. If chunk is not undefined,
    if (!chunk.IsEmpty()) {
      //   a. Assert: chunk.[[ByteLength]] is 0.
      //   b. If byobCanceled is false, perform !
      //      ReadableByteStreamControllerRespondWithNewView(
      //      byobBranch.[[controller]], chunk).
      if (!byob_canceled) {
        bindings::ExceptionState exception_state(isolate);
        ReadableByteStreamController::RespondWithNewView(
            isolate,
            ByteControllerOf(byob_branch_->readable_stream_controller_), chunk,
            exception_state);
      }
      //   c. If otherCanceled is false and
      //      otherBranch.[[controller]].[[pendingPullIntos]] is not empty,
      //      perform !
      //      ReadableByteStreamControllerRespond(otherBranch.[[controller]],
      //      0).
      ReadableByteStreamController* const other_controller =
          ByteControllerOf(other_branch_->readable_stream_controller_);
      if (!other_canceled && !other_controller->pending_pull_intos_.empty()) {
        bindings::ExceptionState exception_state(isolate);
        ReadableByteStreamController::Respond(isolate, other_controller, 0,
                                              exception_state);
      }
    }
    // 7. If byobCanceled is false or otherCanceled is false, resolve
    //    cancelPromise with undefined.
    if (!byob_canceled || !other_canceled) {
      engine_->cancel_promise_->Resolve(isolate);
    }
  }

  void ErrorSteps(v8::Isolate*, v8::Local<v8::Value>) const override {
    // 1. Set reading to false.
    engine_->reading_ = false;
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(engine_);
    visitor->Trace(byob_branch_);
    visitor->Trace(other_branch_);
    ReadIntoRequest::Trace(visitor);
  }

 private:
  void ChunkStepsBody(v8::Global<v8::ArrayBufferView> chunk_global) const {
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    const v8::Local<v8::ArrayBufferView> chunk = chunk_global.Get(isolate);
    // 1. Set readAgainForBranch1 to false.
    engine_->read_again_for_branch_[0] = false;
    // 2. Set readAgainForBranch2 to false.
    engine_->read_again_for_branch_[1] = false;
    // 3. Let byobCanceled be canceled2 if forBranch2 is true, and canceled1
    // otherwise.
    const bool byob_canceled =
        for_branch_2_ ? engine_->canceled_[1] : engine_->canceled_[0];
    // 4. Let otherCanceled be canceled2 if forBranch2 is false, and canceled1
    // otherwise.
    const bool other_canceled =
        !for_branch_2_ ? engine_->canceled_[1] : engine_->canceled_[0];
    // 5. If otherCanceled is false,
    if (!other_canceled) {
      //   a. Let cloneResult be CloneAsUint8Array(chunk).
      const v8::Local<v8::Uint8Array> clone_result =
          engine_->CloneAsUint8Array(isolate, chunk);
      //   b. If cloneResult is an abrupt completion,
      //     i. Perform !
      //     ReadableByteStreamControllerError(byobBranch.[[controller]],
      //     cloneResult.[[Value]]).
      //     ii. Perform !
      //     ReadableByteStreamControllerError(otherBranch.[[controller]],
      //     cloneResult.[[Value]]).
      //     iii. Resolve cancelPromise with !
      //     ReadableStreamCancel(stream, cloneResult.[[Value]]).
      //     iv. Return.
      //   This is not needed as v8::ArrayBuffer::New(), which is used in
      //   CloneAsUint8Array(), crashes if it cannot allocate the memory.

      //   c. Otherwise, let clonedChunk be cloneResult.[[Value]].
      const v8::Local<v8::ArrayBufferView> cloned_chunk = clone_result;

      //   d. If byobCanceled is false, perform !
      //   ReadableByteStreamControllerRespondWithNewView(
      //   byobBranch.[[controller]], chunk).
      if (!byob_canceled) {
        bindings::ExceptionState exception_state(isolate);
        ReadableByteStreamController::RespondWithNewView(
            isolate,
            ByteControllerOf(byob_branch_->readable_stream_controller_), chunk,
            exception_state);
      }
      //   e. Perform !
      //   ReadableByteStreamControllerEnqueue(otherBranch.[[controller]],
      //   clonedChunk).
      bindings::ExceptionState exception_state(isolate);
      ReadableByteStreamController::Enqueue(
          isolate, ByteControllerOf(other_branch_->readable_stream_controller_),
          cloned_chunk, exception_state);
      // 6. Otherwise, if byobCanceled is false, perform !
      // ReadableByteStreamControllerRespondWithNewView(
      // byobBranch.[[controller]], chunk).
    } else if (!byob_canceled) {
      bindings::ExceptionState exception_state(isolate);
      ReadableByteStreamController::RespondWithNewView(
          isolate, ByteControllerOf(byob_branch_->readable_stream_controller_),
          chunk, exception_state);
    }
    // 7. Set reading to false.
    engine_->reading_ = false;
    // 8. If readAgainForBranch1 is true, perform pull1Algorithm.
    if (engine_->read_again_for_branch_[0]) {
      auto* const pull_algorithm =
          bindings::MakeGarbageCollected<PullAlgorithm>(engine_.Get(), 0);
      pull_algorithm->Run(isolate, {});
      // 9. Otherwise, if readAgainForBranch2 is true, perform pull2Algorithm.
    } else if (engine_->read_again_for_branch_[1]) {
      auto* const pull_algorithm =
          bindings::MakeGarbageCollected<PullAlgorithm>(engine_.Get(), 1);
      pull_algorithm->Run(isolate, {});
    }
  }

  const cppgc::Member<ByteStreamTeeEngine> engine_;
  const cppgc::Member<ReadableStream> byob_branch_;
  const cppgc::Member<ReadableStream> other_branch_;
  const bool for_branch_2_;
};

void ByteStreamTeeEngine::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(stream_);
  visitor->Trace(reader_);
  visitor->Trace(cancel_promise_);
  visitor->Trace(reason_[0]);
  visitor->Trace(reason_[1]);
  visitor->Trace(branch_[0]);
  visitor->Trace(branch_[1]);
  visitor->Trace(controller_[0]);
  visitor->Trace(controller_[1]);
}

void ByteStreamTeeEngine::ForwardReaderError(
    v8::Isolate* isolate,
    ReadableStreamGenericReader* this_reader) {
  // 14. Let forwardReaderError be the following steps, taking a thisReader
  // argument:
  class RejectFunction final : public bindings::ScriptFunction {
   public:
    RejectFunction(ByteStreamTeeEngine* engine,
                   ReadableStreamGenericReader* reader)
        : engine_(engine), reader_(reader) {}

    v8::Local<v8::Value> Call(v8::Isolate* isolate,
                              v8::Local<v8::Value> r) override {
      //   a. Upon rejection of thisReader.[[closedPromise]] with reason r,
      //     i. If thisReader is not reader, return.
      if (engine_->reader_.Get() != reader_.Get()) {
        return v8::Undefined(isolate);
      }
      //     ii. Perform !
      //     ReadableByteStreamControllerError(branch1.[[controller]], r).
      ReadableByteStreamController::Error(isolate, engine_->controller_[0], r);
      //     iii. Perform !
      //     ReadableByteStreamControllerError(branch2.[[controller]], r).
      ReadableByteStreamController::Error(isolate, engine_->controller_[1], r);
      //     iv. If canceled1 is false or canceled2 is false, resolve
      //     cancelPromise with undefined.
      if (!engine_->canceled_[0] || !engine_->canceled_[1]) {
        engine_->cancel_promise_->Resolve(isolate);
      }
      return v8::Undefined(isolate);
    }

    void Trace(cppgc::Visitor* visitor) const override {
      visitor->Trace(engine_);
      visitor->Trace(reader_);
      bindings::ScriptFunction::Trace(visitor);
    }

   private:
    const cppgc::Member<ByteStreamTeeEngine> engine_;
    const cppgc::Member<ReadableStreamGenericReader> reader_;
  };

  bindings::PromiseThen(
      isolate, this_reader->Closed(), nullptr,
      bindings::MakeGarbageCollected<RejectFunction>(this, this_reader));
}

void ByteStreamTeeEngine::PullWithDefaultReader(
    v8::Isolate* isolate,
    bindings::ExceptionState& exception_state) {
  // 15. Let pullWithDefaultReader be the following steps:
  //   a. If reader implements ReadableStreamBYOBReader,
  if (reader_->IsBYOBReader()) {
    //     i. Assert: reader.[[readIntoRequests]] is empty.
    ReadableStreamBYOBReader* byob_reader =
        static_cast<ReadableStreamBYOBReader*>(reader_.Get());
    CHECK(byob_reader->read_into_requests_.empty());
    //     ii. Perform ! ReadableStreamBYOBReaderRelease(reader).
    ReadableStreamBYOBReader::Release(isolate, byob_reader);
    //     iii. Set reader to ! AcquireReadableStreamDefaultReader(stream).
    reader_ = ReadableStream::AcquireDefaultReader(isolate, stream_.Get(),
                                                   exception_state);
    //     iv. Perform forwardReaderError, given reader.
    ForwardReaderError(isolate, reader_.Get());
  }
  //   b. Let readRequest be a read request with the following items:
  auto* const read_request =
      bindings::MakeGarbageCollected<ByteTeeReadRequest>(this);
  //   c. Perform ! ReadableStreamDefaultReaderRead(reader, readRequest).
  CHECK(reader_->IsDefaultReader());
  ReadableStreamDefaultReader::Read(
      isolate, static_cast<ReadableStreamDefaultReader*>(reader_.Get()),
      read_request, exception_state);
}

void ByteStreamTeeEngine::PullWithBYOBReader(
    v8::Isolate* isolate,
    v8::Local<v8::ArrayBufferView> view,
    bool for_branch_2,
    bindings::ExceptionState& exception_state) {
  // 16. Let pullWithBYOBReader be the following steps, given view and
  // forBranch2:
  //   a. If reader implements ReadableStreamDefaultReader,
  if (reader_->IsDefaultReader()) {
    //     i. Assert: reader.[[readRequests]] is empty.
    ReadableStreamDefaultReader* default_reader =
        static_cast<ReadableStreamDefaultReader*>(reader_.Get());
    CHECK(default_reader->read_requests_.empty());
    //     ii. Perform ! ReadableStreamDefaultReaderRelease(reader).
    ReadableStreamDefaultReader::Release(isolate, default_reader);
    //     iii. Set reader to ! AcquireReadableStreamBYOBReader(stream).
    reader_ = ReadableStream::AcquireBYOBReader(isolate, stream_.Get(),
                                                exception_state);
    //     iv. Perform forwardReaderError, given reader.
    ForwardReaderError(isolate, reader_.Get());
  }
  //   b. Let byobBranch be branch2 if forBranch2 is true, and branch1
  //   otherwise.
  ReadableStream* byob_branch = for_branch_2 ? branch_[1] : branch_[0];
  //   c. Let otherBranch be branch2 if forBranch2 is false, and branch1
  //   otherwise.
  ReadableStream* other_branch = !for_branch_2 ? branch_[1] : branch_[0];
  //   d. Let readIntoRequest be a read-into request with the following items:
  auto* const read_into_request =
      bindings::MakeGarbageCollected<ByteTeeReadIntoRequest>(
          this, byob_branch, other_branch, for_branch_2);
  //   e. Perform ! ReadableStreamBYOBReaderRead(reader, view, 1,
  //   readIntoRequest).
  CHECK(reader_->IsBYOBReader());
  ReadableStreamBYOBReader::Read(
      isolate, static_cast<ReadableStreamBYOBReader*>(reader_.Get()), view,
      /*min=*/1, read_into_request, exception_state);
}

v8::Local<v8::Uint8Array> ByteStreamTeeEngine::CloneAsUint8Array(
    v8::Isolate* isolate,
    v8::Local<v8::ArrayBufferView> chunk) {
  const size_t byte_length = chunk->ByteLength();
  const v8::Local<v8::ArrayBuffer> cloned_buffer =
      v8::ArrayBuffer::New(isolate, byte_length);
  if (byte_length) {
    chunk->CopyContents(cloned_buffer->Data(), byte_length);
  }
  return v8::Uint8Array::New(cloned_buffer, 0, byte_length);
}

ByteStreamTeeEngine::ByteStreamTeeEngine() = default;

ByteStreamTeeEngine::~ByteStreamTeeEngine() = default;

void ByteStreamTeeEngine::Start(v8::Isolate* isolate,
                                ReadableStream* stream,
                                bindings::ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamtee
  // 1. Assert: stream implements ReadableStream.
  CHECK(stream);

  stream_ = stream;

  // 2. Assert: stream.[[controller]] implements ReadableByteStreamController.
  CHECK(stream->readable_stream_controller_->IsByteStreamController());

  // 3. Let reader be ? AcquireReadableStreamDefaultReader(stream).
  reader_ =
      ReadableStream::AcquireDefaultReader(isolate, stream, exception_state);

  if (exception_state.HadException()) {
    return;
  }
  CHECK(reader_);

  // 4. Let reading be false.
  // 5. Let readAgainForBranch1 be false.
  // 6. Let readAgainForBranch2 be false.
  // 7. Let canceled1 be false.
  // 8. Let canceled2 be false.
  // 9. Let reason1 be undefined.
  // 10. Let reason2 be undefined.
  // 11. Let branch1 be undefined.
  // 12. Let branch2 be undefined.
  // These are the members' initial values.

  // 13. Let cancelPromise be a new promise.
  cancel_promise_ =
      bindings::MakeGarbageCollected<bindings::ScriptPromiseResolver>(isolate);

  // 17. Let pull1Algorithm be the following steps:
  // (See PullAlgorithm::Run()).
  auto* const pull1_algorithm =
      bindings::MakeGarbageCollected<PullAlgorithm>(this, 0);

  // 18. Let pull2Algorithm be the following steps:
  // (both algorithms share a single implementation).
  auto* const pull2_algorithm =
      bindings::MakeGarbageCollected<PullAlgorithm>(this, 1);

  // 19. Let cancel1Algorithm be the following steps, taking a reason argument:
  // (See CancelAlgorithm::Run()).
  auto* const cancel1_algorithm =
      bindings::MakeGarbageCollected<CancelAlgorithm>(this, 0);

  // 20. Let cancel2Algorithm be the following steps, taking a reason argument:
  // (both algorithms share a single implementation).
  auto* const cancel2_algorithm =
      bindings::MakeGarbageCollected<CancelAlgorithm>(this, 1);

  // 21. Let startAlgorithm be an algorithm that returns undefined.
  StreamStartAlgorithm* const start_algorithm = CreateTrivialStartAlgorithm();

  // 22. Set branch1 to ! CreateReadableByteStream(startAlgorithm,
  // pull1Algorithm, cancel1Algorithm).
  branch_[0] = ReadableStream::CreateByteStream(
      isolate, start_algorithm, pull1_algorithm, cancel1_algorithm,
      exception_state);
  if (exception_state.HadException()) {
    return;
  }

  // 23. Set branch2 to ! CreateReadableByteStream(startAlgorithm,
  // pull2Algorithm, cancel2Algorithm).
  branch_[1] = ReadableStream::CreateByteStream(
      isolate, start_algorithm, pull2_algorithm, cancel2_algorithm,
      exception_state);
  if (exception_state.HadException()) {
    return;
  }

  for (int branch = 0; branch < 2; ++branch) {
    // We just created the branches above. It is obvious that they are byte
    // stream controllers.
    controller_[branch] =
        ByteControllerOf(branch_[branch]->readable_stream_controller_);
  }

  // 24. Perform forwardReaderError, given reader.
  ForwardReaderError(isolate, reader_.Get());

  // Step 25. Return « branch1, branch2 ».
  // is performed by the caller.
}

}  // namespace mitto
