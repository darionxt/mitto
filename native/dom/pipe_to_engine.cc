#include "mitto/dom/pipe_to_engine.h"

#include <stddef.h>

#include <optional>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "base/functional/bind.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_function.h"
#include "mitto/bindings/script_promise.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/abort_signal.h"
#include "mitto/dom/miscellaneous_operations.h"
#include "mitto/dom/pipe_options.h"
#include "mitto/dom/read_request.h"
#include "mitto/dom/readable_stream.h"
#include "mitto/dom/readable_stream_byob_reader.h"
#include "mitto/dom/readable_stream_default_reader.h"
#include "mitto/dom/readable_stream_generic_reader.h"
#include "mitto/dom/writable_stream.h"
#include "mitto/dom/writable_stream_default_writer.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

namespace {

// Blink's PromiseAll<IDLUndefined>::GetPromiseForWaitingForAll(), Web IDL's
// "getting a promise to wait for all": a promise resolved with undefined once
// each of the promises has fulfilled, or rejected with the reason of the first
// that is rejected, and resolved from a microtask for none.
class PromiseAllWaiter final
    : public cppgc::GarbageCollected<PromiseAllWaiter> {
 public:
  static v8::Local<v8::Promise> GetPromiseForWaitingForAll(
      v8::Isolate* isolate,
      const std::vector<v8::Local<v8::Promise>>& promises);

  PromiseAllWaiter(v8::Isolate* isolate, size_t number_of_pending_promises)
      : resolver_(
            bindings::MakeGarbageCollected<bindings::ScriptPromiseResolver>(
                isolate)),
        number_of_pending_promises_(number_of_pending_promises) {}

  void OnFulfilled(v8::Isolate* isolate) {
    if (is_settled_) {
      return;
    }
    if (--number_of_pending_promises_ > 0) {
      return;
    }
    RunResolveCallback(isolate);
  }

  void RunResolveCallback(v8::Isolate* isolate) {
    CHECK_EQ(number_of_pending_promises_, 0u);
    is_settled_ = true;
    resolver_->Resolve(isolate);
  }

  void OnRejected(v8::Isolate* isolate, v8::Local<v8::Value> value) {
    if (is_settled_) {
      return;
    }
    is_settled_ = true;
    resolver_->Reject(isolate, value);
  }

  void Trace(cppgc::Visitor* visitor) const { visitor->Trace(resolver_); }

 private:
  const cppgc::Member<bindings::ScriptPromiseResolver> resolver_;
  size_t number_of_pending_promises_;
  bool is_settled_ = false;
};

class PromiseAllResolveFunction final : public bindings::ScriptFunction {
 public:
  explicit PromiseAllResolveFunction(PromiseAllWaiter* all) : all_(all) {}

  v8::Local<v8::Value> Call(v8::Isolate* isolate,
                            v8::Local<v8::Value>) override {
    all_->OnFulfilled(isolate);
    return v8::Undefined(isolate);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(all_);
    bindings::ScriptFunction::Trace(visitor);
  }

 private:
  const cppgc::Member<PromiseAllWaiter> all_;
};

class PromiseAllRejectFunction final : public bindings::ScriptFunction {
 public:
  explicit PromiseAllRejectFunction(PromiseAllWaiter* all) : all_(all) {}

  v8::Local<v8::Value> Call(v8::Isolate* isolate,
                            v8::Local<v8::Value> exception) override {
    all_->OnRejected(isolate, exception);
    return v8::Undefined(isolate);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(all_);
    bindings::ScriptFunction::Trace(visitor);
  }

 private:
  const cppgc::Member<PromiseAllWaiter> all_;
};

// static
v8::Local<v8::Promise> PromiseAllWaiter::GetPromiseForWaitingForAll(
    v8::Isolate* isolate,
    const std::vector<v8::Local<v8::Promise>>& promises) {
  auto* const all = bindings::MakeGarbageCollected<PromiseAllWaiter>(
      isolate, promises.size());
  if (promises.empty()) {
    EnqueueMicrotask(isolate,
                     base::BindOnce(
                         [](cppgc::Persistent<PromiseAllWaiter> all) {
                           all->RunResolveCallback(v8::Isolate::GetCurrent());
                         },
                         cppgc::Persistent<PromiseAllWaiter>(all)));
  } else {
    for (const v8::Local<v8::Promise> promise : promises) {
      bindings::PromiseThen(
          isolate, promise,
          bindings::MakeGarbageCollected<PromiseAllResolveFunction>(all),
          bindings::MakeGarbageCollected<PromiseAllRejectFunction>(all));
    }
  }
  return all->resolver_->Promise(isolate);
}

}  // namespace

class PipeToEngine::PipeToAbortAlgorithm final : public AbortSignal::Algorithm {
 public:
  PipeToAbortAlgorithm(PipeToEngine* engine, AbortSignal* signal)
      : engine_(engine), signal_(signal) {}
  ~PipeToAbortAlgorithm() override = default;

  void Run() override { engine_->AbortAlgorithm(signal_); }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(engine_);
    visitor->Trace(signal_);
    AbortSignal::Algorithm::Trace(visitor);
  }

 private:
  cppgc::Member<PipeToEngine> engine_;
  cppgc::Member<AbortSignal> signal_;
};

class PipeToEngine::PipeToReadRequest final : public ReadRequest {
 public:
  explicit PipeToReadRequest(PipeToEngine* instance) : instance_(instance) {}

  void ChunkSteps(v8::Isolate* isolate,
                  v8::Local<v8::Value> chunk,
                  bindings::ExceptionState&) const override {
    v8::Global<v8::Value> value(isolate, chunk);
    EnqueueMicrotask(isolate,
                     base::BindOnce(
                         [](cppgc::Persistent<PipeToEngine> engine,
                            v8::Global<v8::Value> value) {
                           engine->ReadRequestChunkStepsBody(std::move(value));
                         },
                         cppgc::Persistent<PipeToEngine>(instance_.Get()),
                         std::move(value)));
  }

  void CloseSteps(v8::Isolate*) const override { instance_->ReadableClosed(); }

  void ErrorSteps(v8::Isolate* isolate, v8::Local<v8::Value> e) const override {
    instance_->is_reading_ = false;
    if (instance_->is_shutting_down_) {
      // This function can be called during shutdown when the lock is
      // released. Exit early in that case.
      return;
    }
    instance_->ReadableError(instance_->Readable()->GetStoredError(isolate));
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(instance_);
    ReadRequest::Trace(visitor);
  }

 private:
  cppgc::Member<PipeToEngine> instance_;
};

// This implementation reacts to promises extensively. Instead of a dozen
// separate ScriptFunction subclasses, it uses a single resolve and a single
// reject implementation, and passes a method pointer at runtime to control
// the behaviour.
class PipeToEngine::WrappedPromiseResolve final
    : public bindings::ScriptFunction {
 public:
  using PromiseResolveReaction = void (PipeToEngine::*)();

  WrappedPromiseResolve(PipeToEngine* instance, PromiseResolveReaction method)
      : instance_(instance), method_(method) {}
  // A reaction that returns a promise, which the reaction's promise adopts.
  WrappedPromiseResolve(PipeToEngine* instance, Action method)
      : instance_(instance), promise_method_(method) {}

  v8::Local<v8::Value> Call(v8::Isolate* isolate,
                            v8::Local<v8::Value>) override {
    if (promise_method_) {
      return (instance_.Get()->*promise_method_)();
    }
    (instance_.Get()->*method_)();
    return v8::Undefined(isolate);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(instance_);
    bindings::ScriptFunction::Trace(visitor);
  }

 private:
  const cppgc::Member<PipeToEngine> instance_;
  const PromiseResolveReaction method_ = nullptr;
  const Action promise_method_ = nullptr;
};

class PipeToEngine::WrappedPromiseReject final
    : public bindings::ScriptFunction {
 public:
  using PromiseRejectReaction = void (PipeToEngine::*)(v8::Local<v8::Value>);

  WrappedPromiseReject(PipeToEngine* instance, PromiseRejectReaction method)
      : instance_(instance), method_(method) {}

  v8::Local<v8::Value> Call(v8::Isolate* isolate,
                            v8::Local<v8::Value> value) override {
    (instance_.Get()->*method_)(value);
    return v8::Undefined(isolate);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(instance_);
    bindings::ScriptFunction::Trace(visitor);
  }

 private:
  const cppgc::Member<PipeToEngine> instance_;
  const PromiseRejectReaction method_;
};

PipeToEngine::PipeToEngine(v8::Isolate* isolate, PipeOptions* pipe_options)
    : isolate_(isolate), pipe_options_(pipe_options) {}

PipeToEngine::~PipeToEngine() = default;

void PipeToEngine::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(pipe_options_);
  visitor->Trace(reader_);
  visitor->Trace(writer_);
  visitor->Trace(promise_);
  visitor->Trace(abort_algorithm_);
  visitor->Trace(last_write_);
  visitor->Trace(shutdown_error_);
}

v8::Local<v8::Promise> PipeToEngine::Start(
    ReadableStream* readable,
    WritableStream* destination,
    bindings::ExceptionState& exception_state) {
  // 1. Assert: source implements ReadableStream.
  CHECK(readable);

  // 2. Assert: dest implements WritableStream.
  CHECK(destination);

  // Not relevant to C++ implementation:
  // 3. Assert: preventClose, preventAbort, and preventCancel are all
  // booleans.

  // Already done by WebIDL bindings:
  // 4. If signal was not given, let signal be undefined.
  // 5. Assert: either signal is undefined, or signal implements AbortSignal.

  // 6. Assert: ! IsReadableStreamLocked(source) is false.
  CHECK(!ReadableStream::IsLocked(readable));

  // 7. Assert: ! IsWritableStreamLocked(dest) is false.
  CHECK(!WritableStream::IsLocked(destination));

  // 8. If source.[[controller]] implements ReadableByteStreamController, let
  //    reader be ! AcquireReadableStreamBYOBReader(source) or !
  //    AcquireReadableStreamDefaultReader(source), at the user agent's
  //    discretion.
  // 9. Otherwise, let reader be ! AcquireReadableStreamDefaultReader(source).
  reader_ =
      ReadableStream::AcquireDefaultReader(isolate_, readable, exception_state);
  CHECK(!exception_state.HadException());

  // 10. Let writer be ! AcquireWritableStreamDefaultWriter(dest).
  writer_ = WritableStream::AcquireDefaultWriter(isolate_, destination,
                                                 exception_state);
  CHECK(!exception_state.HadException());

  // 11. Set source.[[disturbed]] to true.
  readable->is_disturbed_ = true;

  // 12. Let shuttingDown be false.
  CHECK(!is_shutting_down_);

  // 13. Let promise be a new promise.
  promise_ = bindings::MakeGarbageCollected<bindings::ScriptPromiseResolver>(
      isolate_.get());

  // 14. If signal is not undefined,
  if (AbortSignal* const signal = pipe_options_->Signal()) {
    //   b. If signal is aborted, perform abortAlgorithm and
    //      return promise.
    if (signal->Aborted()) {
      AbortAlgorithm(signal);
      return promise_->Promise(isolate_);
    }

    //   c. Add abortAlgorithm to signal.
    abort_algorithm_ =
        bindings::MakeGarbageCollected<PipeToAbortAlgorithm>(this, signal);
    signal->AddAlgorithm(abort_algorithm_.Get());
  }

  // 15. In parallel ...
  // The rest of the algorithm is described in terms of a series of
  // constraints rather than as explicit steps.
  if (CheckInitialState()) {
    // Need to detect closing and error when we are not reading. This
    // corresponds to the following conditions from the standard:
    //     1. Errors must be propagated forward: if source.[[state]] is or
    //        becomes "errored", ...
    // and
    //     3. Closing must be propagated forward: if source.[[state]] is or
    //        becomes "closed", ...
    bindings::PromiseThen(isolate_, reader_->Closed(),
                          bindings::MakeGarbageCollected<WrappedPromiseResolve>(
                              this, &PipeToEngine::OnReaderClosed),
                          bindings::MakeGarbageCollected<WrappedPromiseReject>(
                              this, &PipeToEngine::ReadableError));

    // Need to detect error when we are not writing. This corresponds to this
    // condition from the standard:
    //    2. Errors must be propagated backward: if dest.[[state]] is or
    //       becomes "errored", ...
    // We do not need to detect closure of the writable end of the pipe,
    // because we have it locked and so it can only be closed by us.
    bindings::PromiseThen(isolate_, writer_->Closed(), nullptr,
                          bindings::MakeGarbageCollected<WrappedPromiseReject>(
                              this, &PipeToEngine::WritableError));

    // Start the main read / write loop.
    HandleNextEvent();
  }

  // 16. Return promise.
  return promise_->Promise(isolate_);
}

bool PipeToEngine::CheckInitialState() {
  const ReadableStream::State state = Readable()->state_;

  // Both streams can be errored or closed. To perform the right action the
  // order of the checks must match the standard: "the following conditions
  // must be applied in order." This method only checks the initial state;
  // detection of state changes elsewhere is done through checking promise
  // reactions.

  // a. Errors must be propagated forward: if source.[[state]] is or
  //    becomes "errored",
  if (state == ReadableStream::kErrored) {
    ReadableError(Readable()->GetStoredError(isolate_));
    return false;
  }

  // 2. Errors must be propagated backward: if dest.[[state]] is or becomes
  //    "errored",
  if (Destination()->IsErrored()) {
    WritableError(Destination()->GetStoredError(isolate_));
    return false;
  }

  // 3. Closing must be propagated forward: if source.[[state]] is or
  //    becomes "closed", then
  if (state == ReadableStream::kClosed) {
    ReadableClosed();
    return false;
  }

  // 4. Closing must be propagated backward: if !
  //    WritableStreamCloseQueuedOrInFlight(dest) is true or dest.[[state]]
  //    is "closed",
  if (Destination()->IsClosingOrClosed()) {
    WritableStartedClosed();
    return false;
  }

  return true;
}

void PipeToEngine::AbortAlgorithm(AbortSignal* signal) {
  // a. Let abortAlgorithm be the following steps:
  //    i. Let error be signal's abort reason.
  const v8::Local<v8::Value> error = signal->Reason();

  // Steps ii. to iv. are implemented in AbortAlgorithmAction.

  //    v. Shutdown with an action consisting of getting a promise to wait for
  //       all of the actions in actions, and with error.
  ShutdownWithAction(&PipeToEngine::AbortAlgorithmAction, error);
}

v8::Local<v8::Promise> PipeToEngine::AbortAlgorithmAction() {
  const v8::Local<v8::Value> error = shutdown_error_.Get(isolate_);

  // ii. Let actions be an empty ordered set.
  std::vector<v8::Local<v8::Promise>> actions;

  // This method runs later than the equivalent steps in the standard. This
  // means that it is safe to do the checks of the state of the destination
  // and source synchronously, simplifying the logic.

  // iii. If preventAbort is false, append the following action to actions:
  //      1. If dest.[[state]] is "writable", return !
  //         WritableStreamAbort(dest, error).
  //      2. Otherwise, return a promise resolved with undefined.
  if (!pipe_options_->PreventAbort() && Destination()->IsWritable()) {
    actions.push_back(WritableStream::Abort(isolate_, Destination(), error));
  }

  //  iv. If preventCancel is false, append the following action action to
  //      actions:
  //      1. If source.[[state]] is "readable", return !
  //         ReadableStreamCancel(source, error).
  //      2. Otherwise, return a promise resolved with undefined.
  if (!pipe_options_->PreventCancel() &&
      ReadableStream::IsReadable(Readable())) {
    actions.push_back(ReadableStream::Cancel(isolate_, Readable(), error));
  }

  return PromiseAllWaiter::GetPromiseForWaitingForAll(isolate_, actions);
}

void PipeToEngine::HandleNextEvent() {
  CHECK(!is_reading_);
  if (is_shutting_down_) {
    return;
  }

  const std::optional<double> desired_size = writer_->GetDesiredSizeInternal();
  if (!desired_size.has_value()) {
    // This can happen if abort() is queued but not yet started when
    // pipeTo() is called. In that case [[storedError]] is not set yet, and
    // we need to wait until it is before we can cancel the pipe. Once
    // [[storedError]] has been set, the rejection handler set on the writer
    // closed promise above will detect it, so all we need to do here is
    // nothing.
    return;
  }

  if (desired_size.value() <= 0) {
    // Need to wait for backpressure to go away.
    bindings::PromiseThen(isolate_, writer_->Ready(),
                          bindings::MakeGarbageCollected<WrappedPromiseResolve>(
                              this, &PipeToEngine::HandleNextEvent),
                          bindings::MakeGarbageCollected<WrappedPromiseReject>(
                              this, &PipeToEngine::WritableError));
    return;
  }

  is_reading_ = true;
  auto* const read_request =
      bindings::MakeGarbageCollected<PipeToReadRequest>(this);
  bindings::ExceptionState pass_through(isolate_);
  ReadableStreamDefaultReader::Read(isolate_, reader_, read_request,
                                    pass_through);
}

void PipeToEngine::ReadRequestChunkStepsBody(v8::Global<v8::Value> chunk) {
  is_reading_ = false;
  if (is_shutting_down_) {
    return;
  }
  bindings::ExceptionState pass_through(isolate_);
  const v8::Local<v8::Promise> write = WritableStreamDefaultWriter::Write(
      isolate_, writer_, chunk.Get(isolate_), pass_through);
  bindings::PromiseThen(isolate_, write, nullptr,
                        bindings::MakeGarbageCollected<WrappedPromiseReject>(
                            this, &PipeToEngine::WritableError));
  last_write_.Reset(isolate_, write);
  HandleNextEvent();
}

void PipeToEngine::OnReaderClosed() {
  if (!is_reading_) {
    ReadableClosed();
  }
}

void PipeToEngine::ReadableError(v8::Local<v8::Value> error) {
  // This function can be called during shutdown when the lock is released.
  // Exit early in that case.
  if (is_shutting_down_) {
    return;
  }

  // a. If preventAbort is false, shutdown with an action of !
  //    WritableStreamAbort(dest, source.[[storedError]]) and with
  //    source.[[storedError]].
  if (!pipe_options_->PreventAbort()) {
    ShutdownWithAction(&PipeToEngine::WritableStreamAbortAction, error);
  } else {
    // b. Otherwise, shutdown with source.[[storedError]].
    Shutdown(error);
  }
}

void PipeToEngine::WritableError(v8::Local<v8::Value> error) {
  // This function can be called during shutdown when the lock is released.
  // Exit early in that case.
  if (is_shutting_down_) {
    return;
  }

  // a. If preventCancel is false, shutdown with an action of !
  //    ReadableStreamCancel(source, dest.[[storedError]]) and with
  //    dest.[[storedError]].
  if (!pipe_options_->PreventCancel()) {
    ShutdownWithAction(&PipeToEngine::ReadableStreamCancelAction, error);
  } else {
    // b. Otherwise, shutdown with dest.[[storedError]].
    Shutdown(error);
  }
}

void PipeToEngine::ReadableClosed() {
  // a. If preventClose is false, shutdown with an action of !
  //    WritableStreamDefaultWriterCloseWithErrorPropagation(writer).
  if (!pipe_options_->PreventClose()) {
    ShutdownWithAction(
        &PipeToEngine::
            WritableStreamDefaultWriterCloseWithErrorPropagationAction,
        v8::MaybeLocal<v8::Value>());
  } else {
    // b. Otherwise, shutdown.
    Shutdown(v8::MaybeLocal<v8::Value>());
  }
}

void PipeToEngine::WritableStartedClosed() {
  // a. Assert: no chunks have been read or written.
  // This is trivially true because this method is only called from
  // CheckInitialState().

  // b. Let destClosed be a new TypeError.
  const v8::Local<v8::Value> dest_closed =
      CreateTypeError(isolate_, "Destination stream closed");

  // c. If preventCancel is false, shutdown with an action of !
  //    ReadableStreamCancel(source, destClosed) and with destClosed.
  if (!pipe_options_->PreventCancel()) {
    ShutdownWithAction(&PipeToEngine::ReadableStreamCancelAction, dest_closed);
  } else {
    // d. Otherwise, shutdown with destClosed.
    Shutdown(dest_closed);
  }
}

void PipeToEngine::ShutdownWithAction(
    Action action,
    v8::MaybeLocal<v8::Value> original_error) {
  // a. If shuttingDown is true, abort these substeps.
  if (is_shutting_down_) {
    return;
  }

  // b. Set shuttingDown to true.
  is_shutting_down_ = true;

  // Store the action in case we need to call it asynchronously. This is safe
  // because the |is_shutting_down_| guard flag ensures that we can only reach
  // this assignment once.
  shutdown_action_ = action;

  // Store |original_error| as |shutdown_error_| if it was supplied.
  v8::Local<v8::Value> original_error_local;
  if (original_error.ToLocal(&original_error_local)) {
    shutdown_error_.Reset(isolate_, original_error_local);
  }
  v8::Local<v8::Promise> p;

  // c. If dest.[[state]] is "writable" and !
  //    WritableStreamCloseQueuedOrInFlight(dest) is false,
  if (ShouldWriteQueuedChunks()) {
    //  i. If any chunks have been read but not yet written, write them to
    //     dest.
    // ii. Wait until every chunk that has been read has been written
    //     (i.e. the corresponding promises have settled).
    p = bindings::PromiseThen(
        isolate_, WriteQueuedChunks(),
        bindings::MakeGarbageCollected<WrappedPromiseResolve>(
            this, &PipeToEngine::InvokeShutdownAction));
  } else {
    // d. Let p be the result of performing action.
    p = InvokeShutdownAction();
  }
  if (p.IsEmpty()) {
    return;
  }

  // e. Upon fulfillment of p, finalize, passing along originalError if it
  //    was given.
  // f. Upon rejection of p with reason newError, finalize with newError.
  bindings::PromiseThen(
      isolate_, p,
      bindings::MakeGarbageCollected<WrappedPromiseResolve>(
          this, &PipeToEngine::FinalizeWithOriginalErrorIfSet),
      bindings::MakeGarbageCollected<WrappedPromiseReject>(
          this, &PipeToEngine::FinalizeWithNewError));
}

void PipeToEngine::Shutdown(v8::MaybeLocal<v8::Value> error_maybe) {
  // a. If shuttingDown is true, abort these substeps.
  if (is_shutting_down_) {
    return;
  }

  // b. Set shuttingDown to true.
  is_shutting_down_ = true;

  // c. If dest.[[state]] is "writable" and !
  //    WritableStreamCloseQueuedOrInFlight(dest) is false,
  if (ShouldWriteQueuedChunks()) {
    // Need to stash the value of |error_maybe| since we are calling
    // Finalize() asynchronously.
    v8::Local<v8::Value> error;
    if (error_maybe.ToLocal(&error)) {
      shutdown_error_.Reset(isolate_, error);
    }

    //  i. If any chunks have been read but not yet written, write them to
    //     dest.
    // ii. Wait until every chunk that has been read has been written
    //     (i.e. the corresponding promises have settled).
    // d. Finalize, passing along error if it was given.
    bindings::PromiseThen(
        isolate_, WriteQueuedChunks(),
        bindings::MakeGarbageCollected<WrappedPromiseResolve>(
            this, &PipeToEngine::FinalizeWithOriginalErrorIfSet));
  } else {
    // d. Finalize, passing along error if it was given.
    Finalize(error_maybe);
  }
}

void PipeToEngine::FinalizeWithOriginalErrorIfSet() {
  v8::MaybeLocal<v8::Value> error_maybe;
  if (!shutdown_error_.IsEmpty()) {
    error_maybe = shutdown_error_.Get(isolate_);
  }
  Finalize(error_maybe);
}

void PipeToEngine::FinalizeWithNewError(v8::Local<v8::Value> new_error) {
  Finalize(new_error);
}

void PipeToEngine::Finalize(v8::MaybeLocal<v8::Value> error_maybe) {
  // a. Perform ! WritableStreamDefaultWriterRelease(writer).
  WritableStreamDefaultWriter::Release(isolate_, writer_);

  // b. If reader implements ReadableStreamBYOBReader, perform !
  // ReadableStreamBYOBReaderRelease(reader).
  if (reader_->IsBYOBReader()) {
    ReadableStreamGenericReader* const reader = reader_.Get();
    ReadableStreamBYOBReader::Release(
        isolate_, static_cast<ReadableStreamBYOBReader*>(reader));
  } else {
    // c. Otherwise, perform ! ReadableStreamDefaultReaderRelease(reader).
    CHECK(reader_->IsDefaultReader());
    ReadableStreamDefaultReader::Release(isolate_, reader_);
  }

  // d. If signal is not undefined, remove abortAlgorithm from signal.
  //
  // An abort algorithm is only added if the signal provided to pipeTo is not
  // undefined *and* not aborted, which means `abort_algorithm_` can be null if
  // signal is not undefined.
  if (abort_algorithm_) {
    AbortSignal* const signal = pipe_options_->Signal();
    CHECK(signal);
    signal->RemoveAlgorithm(abort_algorithm_.Get());
  }

  v8::Local<v8::Value> error;
  if (error_maybe.ToLocal(&error)) {
    // e. If error was given, reject promise with error.
    promise_->Reject(isolate_, error);
  } else {
    // f. Otherwise, resolve promise with undefined.
    promise_->Resolve(isolate_);
  }
}

bool PipeToEngine::ShouldWriteQueuedChunks() const {
  // "If dest.[[state]] is "writable" and !
  // WritableStreamCloseQueuedOrInFlight(dest) is false"
  return Destination()->IsWritable() &&
         !WritableStream::CloseQueuedOrInFlight(Destination());
}

v8::Local<v8::Promise> PipeToEngine::WriteQueuedChunks() {
  if (!last_write_.IsEmpty()) {
    // "Wait until every chunk that has been read has been written (i.e.
    // the corresponding promises have settled)"
    // This implies that we behave the same whether the promise fulfills or
    // rejects. IgnoreErrors() will convert a rejection into a successful
    // resolution.
    return bindings::PromiseThen(
        isolate_, last_write_.Get(isolate_), nullptr,
        bindings::MakeGarbageCollected<WrappedPromiseReject>(
            this, &PipeToEngine::IgnoreErrors));
  }
  return bindings::ResolvedUndefinedPromise(isolate_);
}

v8::Local<v8::Promise> PipeToEngine::WritableStreamAbortAction() {
  return WritableStream::Abort(isolate_, Destination(), ShutdownError());
}

v8::Local<v8::Promise> PipeToEngine::ReadableStreamCancelAction() {
  return ReadableStream::Cancel(isolate_, Readable(), ShutdownError());
}

v8::Local<v8::Promise>
PipeToEngine::WritableStreamDefaultWriterCloseWithErrorPropagationAction() {
  return WritableStreamDefaultWriter::CloseWithErrorPropagation(isolate_,
                                                                writer_);
}

WritableStream* PipeToEngine::Destination() {
  return writer_->OwnerWritableStream();
}

const WritableStream* PipeToEngine::Destination() const {
  return writer_->OwnerWritableStream();
}

ReadableStream* PipeToEngine::Readable() {
  return reader_->OwnerReadableStream();
}

}  // namespace mitto
