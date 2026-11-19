// The Streams Standard's ReadableStreamDefaultController, as Blink's.
//
// Upstream: third_party/blink/renderer/core/streams/readable_stream_default_controller.h
// Upstream: third_party/blink/renderer/core/streams/readable_stream_default_controller.cc

#ifndef MITTO_DOM_READABLE_STREAM_DEFAULT_CONTROLLER_H_
#define MITTO_DOM_READABLE_STREAM_DEFAULT_CONTROLLER_H_

#include <optional>

#include "mitto/dom/readable_stream_controller.h"
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

class QueueWithSizes;
class ReadableStream;
class ReadRequest;
class StrategySizeAlgorithm;
class StreamAlgorithm;
class StreamStartAlgorithm;

class ReadableStreamDefaultController final : public ReadableStreamController {
 public:
  ReadableStreamDefaultController();
  ~ReadableStreamDefaultController() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  std::optional<double> DesiredSize() const { return GetDesiredSize(); }
  void Close(bindings::ExceptionState& exception_state);
  void Enqueue(const std::optional<v8::Local<v8::Value>>& chunk,
               bindings::ExceptionState& exception_state);
  void Error(const std::optional<v8::Local<v8::Value>>& e);

  // ReadableStreamDefaultControllerClose().
  static void Close(v8::Isolate* isolate,
                    ReadableStreamDefaultController* controller);
  // ReadableStreamDefaultControllerEnqueue().
  static void Enqueue(v8::Isolate* isolate,
                      ReadableStreamDefaultController* controller,
                      v8::Local<v8::Value> chunk,
                      bindings::ExceptionState& exception_state);
  // ReadableStreamDefaultControllerError().
  static void Error(v8::Isolate* isolate,
                    ReadableStreamDefaultController* controller,
                    v8::Local<v8::Value> e);
  // ReadableStreamDefaultControllerGetDesiredSize().
  std::optional<double> GetDesiredSize() const;

  // For TransformStream.
  //
  // ReadableStreamDefaultControllerCanCloseOrEnqueue().
  static bool CanCloseOrEnqueue(
      const ReadableStreamDefaultController* controller);
  // ReadableStreamDefaultControllerHasBackpressure().
  static bool HasBackpressure(
      const ReadableStreamDefaultController* controller);
  static const char* EnqueueExceptionMessage(
      const ReadableStreamDefaultController* controller);

  bool IsDefaultController() const override;
  bool IsByteStreamController() const override;

  // [[CancelSteps]], [[PullSteps]] and [[ReleaseSteps]].
  v8::Local<v8::Promise> CancelSteps(v8::Isolate* isolate,
                                     v8::Local<v8::Value> reason) override;
  void PullSteps(v8::Isolate* isolate,
                 ReadRequest* read_request,
                 bindings::ExceptionState& exception_state) override;
  void ReleaseSteps() override;

 private:
  friend class ReadableStream;
  friend class ReadableStreamDefaultReader;

  class CallPullIfNeededResolveFunction;
  class CallPullIfNeededRejectFunction;

  // ReadableStreamDefaultControllerCallPullIfNeeded().
  static void CallPullIfNeeded(v8::Isolate* isolate,
                               ReadableStreamDefaultController* controller);
  // ReadableStreamDefaultControllerShouldCallPull().
  static bool ShouldCallPull(const ReadableStreamDefaultController* controller);
  // ReadableStreamDefaultControllerClearAlgorithms().
  static void ClearAlgorithms(ReadableStreamDefaultController* controller);
  // SetUpReadableStreamDefaultController().
  static void SetUp(v8::Isolate* isolate,
                    ReadableStream* stream,
                    ReadableStreamDefaultController* controller,
                    StreamStartAlgorithm* start_algorithm,
                    StreamAlgorithm* pull_algorithm,
                    StreamAlgorithm* cancel_algorithm,
                    double high_water_mark,
                    StrategySizeAlgorithm* size_algorithm,
                    bindings::ExceptionState& exception_state);
  // SetUpReadableStreamDefaultControllerFromUnderlyingSource().
  static void SetUpFromUnderlyingSource(
      v8::Isolate* isolate,
      ReadableStream* stream,
      v8::Local<v8::Object> underlying_source,
      double high_water_mark,
      StrategySizeAlgorithm* size_algorithm,
      bindings::ExceptionState& exception_state);

  bool is_close_requested_ = false;
  bool will_pull_again_ = false;
  bool is_pulling_ = false;
  bool is_started_ = false;
  cppgc::Member<StreamAlgorithm> cancel_algorithm_;
  cppgc::Member<ReadableStream> controlled_readable_stream_;
  cppgc::Member<StreamAlgorithm> pull_algorithm_;
  cppgc::Member<QueueWithSizes> queue_;
  double strategy_high_water_mark_ = 0.0;
  cppgc::Member<StrategySizeAlgorithm> strategy_size_algorithm_;
  cppgc::Member<CallPullIfNeededResolveFunction> resolve_function_;
  cppgc::Member<CallPullIfNeededRejectFunction> reject_function_;
};

}  // namespace mitto

#endif  // MITTO_DOM_READABLE_STREAM_DEFAULT_CONTROLLER_H_
