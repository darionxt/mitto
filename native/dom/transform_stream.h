// The Streams Standard's TransformStream, as Blink's: of a page's transformer
// or of the Runtime's own.
//
// Upstream: third_party/blink/renderer/core/streams/transform_stream.h
// Upstream: third_party/blink/renderer/core/streams/transform_stream.cc

#ifndef MITTO_DOM_TRANSFORM_STREAM_H_
#define MITTO_DOM_TRANSFORM_STREAM_H_

#include <optional>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"

namespace v8 {
class Isolate;
class Value;
}  // namespace v8

namespace mitto {

namespace bindings {
class ExceptionState;
class ScriptPromiseResolver;
}  // namespace bindings

class ReadableStream;
class ReadableStreamDefaultController;
class StrategySizeAlgorithm;
class StreamAlgorithm;
class StreamStartAlgorithm;
class TransformStreamDefaultController;
class TransformStreamTransformer;
class WritableStream;

class TransformStream final : public bindings::ScriptWrappable {
 public:
  // `new TransformStream(transformer, writableStrategy, readableStrategy)`.
  static TransformStream* Create(
      const std::optional<v8::Local<v8::Value>>& transformer,
      const std::optional<v8::Local<v8::Value>>& writable_strategy,
      const std::optional<v8::Local<v8::Value>>& readable_strategy,
      bindings::ExceptionState& exception_state);

  // One of the Runtime's own transformer.
  static TransformStream* Create(v8::Isolate* isolate,
                                 TransformStreamTransformer* transformer,
                                 bindings::ExceptionState& exception_state);

  // CreateTransformStream().
  static TransformStream* Create(
      v8::Isolate* isolate,
      StreamStartAlgorithm* start_algorithm,
      StreamAlgorithm* transform_algorithm,
      StreamAlgorithm* flush_algorithm,
      double writable_high_water_mark,
      StrategySizeAlgorithm* writable_size_algorithm,
      double readable_high_water_mark,
      StrategySizeAlgorithm* readable_size_algorithm,
      bindings::ExceptionState& exception_state);

  TransformStream();
  // One of an existing pair, which needs no initialization.
  TransformStream(ReadableStream* readable, WritableStream* writable);
  ~TransformStream() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  ReadableStream* Readable() const { return readable_.Get(); }
  WritableStream* Writable() const { return writable_.Get(); }

 private:
  friend class TransformStreamDefaultController;

  class ControllerInterface;
  class FlushAlgorithm;
  class TransformAlgorithm;
  class ReturnStartPromiseAlgorithm;
  class DefaultSinkWriteAlgorithm;
  class DefaultSinkAbortAlgorithm;
  class DefaultSinkCloseAlgorithm;
  class DefaultSourcePullAlgorithm;
  class DefaultSourceCancelAlgorithm;

  // The steps of the constructor.
  void InitInternal(v8::Isolate* isolate,
                    v8::Local<v8::Value> raw_transformer,
                    v8::Local<v8::Value> raw_writable_strategy,
                    v8::Local<v8::Value> raw_readable_strategy,
                    bindings::ExceptionState& exception_state);

  // InitializeTransformStream().
  static void Initialize(v8::Isolate* isolate,
                         TransformStream* stream,
                         bindings::ScriptPromiseResolver* start_promise,
                         double writable_high_water_mark,
                         StrategySizeAlgorithm* writable_size_algorithm,
                         double readable_high_water_mark,
                         StrategySizeAlgorithm* readable_size_algorithm,
                         bindings::ExceptionState& exception_state);
  // TransformStreamError().
  static void Error(v8::Isolate* isolate,
                    TransformStream* stream,
                    v8::Local<v8::Value> e);
  // TransformStreamErrorWritableAndUnblockWrite().
  static void ErrorWritableAndUnblockWrite(v8::Isolate* isolate,
                                           TransformStream* stream,
                                           v8::Local<v8::Value> e);
  // TransformStreamSetBackpressure().
  static void SetBackpressure(v8::Isolate* isolate,
                              TransformStream* stream,
                              bool backpressure);

  ReadableStreamDefaultController* GetReadableController();

  // [[backpressure]], which InitializeTransformStream() sets to true.
  bool had_backpressure_ = true;
  cppgc::Member<bindings::ScriptPromiseResolver> backpressure_change_promise_;
  cppgc::Member<ReadableStream> readable_;
  cppgc::Member<TransformStreamDefaultController> transform_stream_controller_;
  cppgc::Member<WritableStream> writable_;
};

}  // namespace mitto

#endif  // MITTO_DOM_TRANSFORM_STREAM_H_
