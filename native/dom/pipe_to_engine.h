// The Streams Standard's ReadableStreamPipeTo(), as Blink's PipeToEngine: a
// state machine that promises drive.
//
// Upstream: third_party/blink/renderer/core/streams/pipe_to_engine.h
// Upstream: third_party/blink/renderer/core/streams/pipe_to_engine.cc

#ifndef MITTO_DOM_PIPE_TO_ENGINE_H_
#define MITTO_DOM_PIPE_TO_ENGINE_H_

#include "base/check.h"
#include "base/memory/raw_ptr.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-maybe.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-traced-handle.h"

namespace v8 {
class Isolate;
class Value;
}  // namespace v8

namespace mitto {

namespace bindings {
class ExceptionState;
class ScriptPromiseResolver;
}  // namespace bindings

class AbortSignal;
class PipeOptions;
class ReadableStream;
class ReadableStreamDefaultReader;
class WritableStream;
class WritableStreamDefaultWriter;

class PipeToEngine final : public cppgc::GarbageCollected<PipeToEngine> {
 public:
  PipeToEngine(v8::Isolate* isolate, PipeOptions* pipe_options);
  PipeToEngine(const PipeToEngine&) = delete;
  PipeToEngine& operator=(const PipeToEngine&) = delete;
  ~PipeToEngine();

  // ReadableStreamPipeTo().
  v8::Local<v8::Promise> Start(ReadableStream* readable,
                               WritableStream* destination,
                               bindings::ExceptionState& exception_state);

  void Trace(cppgc::Visitor* visitor) const;

 private:
  class PipeToAbortAlgorithm;
  class PipeToReadRequest;
  class WrappedPromiseResolve;
  class WrappedPromiseReject;

  // An action of "shutdown with an action", a method that calls an abstract
  // operation with what this engine knows.
  using Action = v8::Local<v8::Promise> (PipeToEngine::*)();

  // Checks the streams' states and runs the shutdown steps if needed. Returns
  // whether piping can go on.
  bool CheckInitialState();
  void AbortAlgorithm(AbortSignal* signal);
  v8::Local<v8::Promise> AbortAlgorithmAction();
  // HandleNextEvent() and the read's chunk steps call each other
  // asynchronously until the pipe ends.
  void HandleNextEvent();
  void ReadRequestChunkStepsBody(v8::Global<v8::Value> chunk);
  // If a read is in progress, waits for it to say that the stream closed, so
  // that all of the data is written before shutdown.
  void OnReaderClosed();
  // Errors must be propagated forward.
  void ReadableError(v8::Local<v8::Value> error);
  // Errors must be propagated backward.
  void WritableError(v8::Local<v8::Value> error);
  // Closing must be propagated forward.
  void ReadableClosed();
  // Closing must be propagated backward.
  void WritableStartedClosed();
  // "Shutdown with an action".
  void ShutdownWithAction(Action action,
                          v8::MaybeLocal<v8::Value> original_error);
  // "Shutdown".
  void Shutdown(v8::MaybeLocal<v8::Value> error_maybe);
  // Finalize() with the stored shutdown error rather than what was passed.
  void FinalizeWithOriginalErrorIfSet();
  // Finalize() with what was passed as the error.
  void FinalizeWithNewError(v8::Local<v8::Value> new_error);
  // "Finalize".
  void Finalize(v8::MaybeLocal<v8::Value> error_maybe);
  bool ShouldWriteQueuedChunks() const;
  v8::Local<v8::Promise> WriteQueuedChunks();
  void IgnoreErrors(v8::Local<v8::Value>) {}
  v8::Local<v8::Promise> InvokeShutdownAction() {
    return (this->*shutdown_action_)();
  }
  v8::Local<v8::Value> ShutdownError() const {
    CHECK(!shutdown_error_.IsEmpty());
    return shutdown_error_.Get(isolate_);
  }
  v8::Local<v8::Promise> WritableStreamAbortAction();
  v8::Local<v8::Promise> ReadableStreamCancelAction();
  v8::Local<v8::Promise>
  WritableStreamDefaultWriterCloseWithErrorPropagationAction();
  WritableStream* Destination();
  const WritableStream* Destination() const;
  ReadableStream* Readable();

  const raw_ptr<v8::Isolate> isolate_;
  cppgc::Member<PipeOptions> pipe_options_;
  cppgc::Member<ReadableStreamDefaultReader> reader_;
  cppgc::Member<WritableStreamDefaultWriter> writer_;
  cppgc::Member<bindings::ScriptPromiseResolver> promise_;
  // The abort algorithm added to the signal, as Blink's handle of it.
  cppgc::Member<PipeToAbortAlgorithm> abort_algorithm_;
  v8::TracedReference<v8::Promise> last_write_;
  Action shutdown_action_ = nullptr;
  v8::TracedReference<v8::Value> shutdown_error_;
  bool is_shutting_down_ = false;
  bool is_reading_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_PIPE_TO_ENGINE_H_
