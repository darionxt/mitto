// What a ReadableStream asks of its controller, a default controller's or a
// byte stream controller's, as Blink's ReadableStreamController.
//
// Upstream: third_party/blink/renderer/core/streams/readable_stream_controller.h

#ifndef MITTO_DOM_READABLE_STREAM_CONTROLLER_H_
#define MITTO_DOM_READABLE_STREAM_CONTROLLER_H_

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-promise.h"

namespace v8 {
class Isolate;
class Value;
}  // namespace v8

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class ReadRequest;

class ReadableStreamController : public bindings::ScriptWrappable {
 public:
  virtual bool IsDefaultController() const = 0;
  virtual bool IsByteStreamController() const = 0;

  // [[CancelSteps]].
  virtual v8::Local<v8::Promise> CancelSteps(v8::Isolate* isolate,
                                             v8::Local<v8::Value> reason) = 0;
  // [[PullSteps]].
  virtual void PullSteps(v8::Isolate* isolate,
                         ReadRequest* read_request,
                         bindings::ExceptionState& exception_state) = 0;
  // [[ReleaseSteps]].
  virtual void ReleaseSteps() = 0;
};

}  // namespace mitto

#endif  // MITTO_DOM_READABLE_STREAM_CONTROLLER_H_
