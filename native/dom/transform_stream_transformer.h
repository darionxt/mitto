// What the Runtime's own transformer gives a TransformStream, such as
// TextDecoderStream's decoder, after the Streams Standard's transformer, as
// Blink's TransformStreamTransformer. It signals an error by throwing or by
// rejecting.
//
// Upstream: third_party/blink/renderer/core/streams/transform_stream_transformer.h

#ifndef MITTO_DOM_TRANSFORM_STREAM_TRANSFORMER_H_
#define MITTO_DOM_TRANSFORM_STREAM_TRANSFORMER_H_

#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-promise.h"

namespace v8 {
class Value;
}  // namespace v8

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class TransformStreamDefaultController;

class TransformStreamTransformer
    : public cppgc::GarbageCollected<TransformStreamTransformer> {
 public:
  TransformStreamTransformer() = default;
  TransformStreamTransformer(const TransformStreamTransformer&) = delete;
  TransformStreamTransformer& operator=(const TransformStreamTransformer&) =
      delete;
  virtual ~TransformStreamTransformer() = default;

  virtual v8::Local<v8::Promise> Transform(
      v8::Local<v8::Value> chunk,
      TransformStreamDefaultController* controller,
      bindings::ExceptionState& exception_state) = 0;
  virtual v8::Local<v8::Promise> Flush(
      TransformStreamDefaultController* controller,
      bindings::ExceptionState& exception_state) = 0;

  virtual void Trace(cppgc::Visitor* visitor) const {}
};

}  // namespace mitto

#endif  // MITTO_DOM_TRANSFORM_STREAM_TRANSFORMER_H_
