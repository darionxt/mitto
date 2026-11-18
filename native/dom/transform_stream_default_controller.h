// The Streams Standard's TransformStreamDefaultController, as Blink's.
//
// Upstream: third_party/blink/renderer/core/streams/transform_stream_default_controller.h
// Upstream: third_party/blink/renderer/core/streams/transform_stream_default_controller.cc

#ifndef MITTO_DOM_TRANSFORM_STREAM_DEFAULT_CONTROLLER_H_
#define MITTO_DOM_TRANSFORM_STREAM_DEFAULT_CONTROLLER_H_

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

class ReadableStreamDefaultController;
class StreamAlgorithm;
class TransformStream;

class TransformStreamDefaultController final
    : public bindings::ScriptWrappable {
 public:
  TransformStreamDefaultController();
  ~TransformStreamDefaultController() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  std::optional<double> DesiredSize() const;
  void Enqueue(const std::optional<v8::Local<v8::Value>>& chunk,
               bindings::ExceptionState& exception_state);
  void Error(const std::optional<v8::Local<v8::Value>>& reason);
  void Terminate();

  // TransformStreamDefaultControllerEnqueue(), for the Runtime's own
  // transformers.
  static void Enqueue(v8::Isolate* isolate,
                      TransformStreamDefaultController* controller,
                      v8::Local<v8::Value> chunk,
                      bindings::ExceptionState& exception_state);

 private:
  friend class TransformStream;

  class DefaultTransformAlgorithm;
  class PerformTransformRejectFunction;

  // SetUpTransformStreamDefaultController().
  static void SetUp(v8::Isolate* isolate,
                    TransformStream* stream,
                    TransformStreamDefaultController* controller,
                    StreamAlgorithm* transform_algorithm,
                    StreamAlgorithm* flush_algorithm);
  // SetUpTransformStreamDefaultControllerFromTransformer(): the controller's
  // value, or an empty handle if it threw.
  static v8::Local<v8::Value> SetUpFromTransformer(
      v8::Isolate* isolate,
      TransformStream* stream,
      v8::Local<v8::Object> transformer,
      bindings::ExceptionState& exception_state);
  // TransformStreamDefaultControllerClearAlgorithms().
  static void ClearAlgorithms(TransformStreamDefaultController* controller);
  // TransformStreamDefaultControllerError().
  static void Error(v8::Isolate* isolate,
                    TransformStreamDefaultController* controller,
                    v8::Local<v8::Value> e);
  // TransformStreamDefaultControllerPerformTransform().
  static v8::Local<v8::Promise> PerformTransform(
      v8::Isolate* isolate,
      TransformStreamDefaultController* controller,
      v8::Local<v8::Value> chunk);
  // TransformStreamDefaultControllerTerminate().
  static void Terminate(v8::Isolate* isolate,
                        TransformStreamDefaultController* controller);
  static ReadableStreamDefaultController* GetDefaultController(
      TransformStream* stream);

  cppgc::Member<TransformStream> controlled_transform_stream_;
  cppgc::Member<StreamAlgorithm> flush_algorithm_;
  cppgc::Member<StreamAlgorithm> transform_algorithm_;
  cppgc::Member<PerformTransformRejectFunction> reject_function_;
};

}  // namespace mitto

#endif  // MITTO_DOM_TRANSFORM_STREAM_DEFAULT_CONTROLLER_H_
