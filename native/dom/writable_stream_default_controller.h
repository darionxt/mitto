// The Streams Standard's WritableStreamDefaultController, as Blink's.
//
// Upstream: third_party/blink/renderer/core/streams/writable_stream_default_controller.h
// Upstream: third_party/blink/renderer/core/streams/writable_stream_default_controller.cc

#ifndef MITTO_DOM_WRITABLE_STREAM_DEFAULT_CONTROLLER_H_
#define MITTO_DOM_WRITABLE_STREAM_DEFAULT_CONTROLLER_H_

#include <optional>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-promise.h"

namespace v8 {
class Isolate;
class Value;
}  // namespace v8

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class AbortController;
class AbortSignal;
class QueueWithSizes;
class StrategySizeAlgorithm;
class StreamAlgorithm;
class StreamStartAlgorithm;
class WritableStream;

class WritableStreamDefaultController final : public bindings::ScriptWrappable {
 public:
  WritableStreamDefaultController();
  ~WritableStreamDefaultController() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  AbortSignal* Signal() const;
  void Error(const std::optional<v8::Local<v8::Value>>& e);

  // For WritableStream.
  //
  // [[AbortSteps]].
  v8::Local<v8::Promise> AbortSteps(v8::Isolate* isolate,
                                    v8::Local<v8::Value> reason);
  // [[ErrorSteps]].
  void ErrorSteps();
  // SetUpWritableStreamDefaultController().
  static void SetUp(v8::Isolate* isolate,
                    WritableStream* stream,
                    WritableStreamDefaultController* controller,
                    StreamStartAlgorithm* start_algorithm,
                    StreamAlgorithm* write_algorithm,
                    StreamAlgorithm* close_algorithm,
                    StreamAlgorithm* abort_algorithm,
                    double high_water_mark,
                    StrategySizeAlgorithm* size_algorithm,
                    bindings::ExceptionState& exception_state);
  // SetUpWritableStreamDefaultControllerFromUnderlyingSink().
  static void SetUpFromUnderlyingSink(
      v8::Isolate* isolate,
      WritableStream* stream,
      v8::Local<v8::Object> underlying_sink,
      double high_water_mark,
      StrategySizeAlgorithm* size_algorithm,
      bindings::ExceptionState& exception_state);

  // For WritableStreamDefaultWriter.
  //
  // WritableStreamDefaultControllerClose().
  static void Close(v8::Isolate* isolate,
                    WritableStreamDefaultController* controller);
  // WritableStreamDefaultControllerGetChunkSize(), which may error the
  // stream.
  static double GetChunkSize(v8::Isolate* isolate,
                             WritableStreamDefaultController* controller,
                             v8::Local<v8::Value> chunk);
  // WritableStreamDefaultControllerGetDesiredSize().
  static double GetDesiredSize(
      const WritableStreamDefaultController* controller);
  // WritableStreamDefaultControllerWrite().
  static void Write(v8::Isolate* isolate,
                    WritableStreamDefaultController* controller,
                    v8::Local<v8::Value> chunk,
                    double chunk_size,
                    bindings::ExceptionState& exception_state);
  // WritableStreamDefaultControllerError().
  static void Error(v8::Isolate* isolate,
                    WritableStreamDefaultController* controller,
                    v8::Local<v8::Value> error);

  // Not part of the standard.
  bool Started() const { return started_; }

  // For TransformStream.
  //
  // WritableStreamDefaultControllerErrorIfNeeded().
  static void ErrorIfNeeded(v8::Isolate* isolate,
                            WritableStreamDefaultController* controller,
                            v8::Local<v8::Value> error);

  // Signals abort on the controller's signal with `reason`.
  void Abort(v8::Isolate* isolate, v8::Local<v8::Value> reason);

 private:
  class ProcessWriteResolveFunction;
  class ProcessWriteRejectFunction;

  // WritableStreamDefaultControllerClearAlgorithms().
  static void ClearAlgorithms(WritableStreamDefaultController* controller);
  // WritableStreamDefaultControllerAdvanceQueueIfNeeded().
  static void AdvanceQueueIfNeeded(v8::Isolate* isolate,
                                   WritableStreamDefaultController* controller);
  // WritableStreamDefaultControllerProcessClose().
  static void ProcessClose(v8::Isolate* isolate,
                           WritableStreamDefaultController* controller);
  // WritableStreamDefaultControllerProcessWrite().
  static void ProcessWrite(v8::Isolate* isolate,
                           WritableStreamDefaultController* controller,
                           v8::Local<v8::Value> chunk);
  // WritableStreamDefaultControllerGetBackpressure().
  static bool GetBackpressure(
      const WritableStreamDefaultController* controller);

  cppgc::Member<StreamAlgorithm> abort_algorithm_;
  cppgc::Member<StreamAlgorithm> close_algorithm_;
  cppgc::Member<WritableStream> controlled_writable_stream_;
  // [[queue]] and [[queueTotalSize]]. Chunks are kept as they are, and the
  // "close" marker is an empty queue with `close_queued_` set.
  cppgc::Member<QueueWithSizes> queue_;
  cppgc::Member<AbortController> abort_controller_;
  bool close_queued_ = false;
  bool started_ = false;
  double strategy_high_water_mark_ = 0.0;
  cppgc::Member<StrategySizeAlgorithm> strategy_size_algorithm_;
  cppgc::Member<StreamAlgorithm> write_algorithm_;
  cppgc::Member<ProcessWriteResolveFunction> resolve_function_;
  cppgc::Member<ProcessWriteRejectFunction> reject_function_;
};

}  // namespace mitto

#endif  // MITTO_DOM_WRITABLE_STREAM_DEFAULT_CONTROLLER_H_
