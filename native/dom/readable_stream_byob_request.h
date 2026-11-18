// The Streams Standard's ReadableStreamBYOBRequest, as Blink's: the view that
// a byte stream's source fills for a pending read.
//
// Upstream: third_party/blink/renderer/core/streams/readable_stream_byob_request.h
// Upstream: third_party/blink/renderer/core/streams/readable_stream_byob_request.cc

#ifndef MITTO_DOM_READABLE_STREAM_BYOB_REQUEST_H_
#define MITTO_DOM_READABLE_STREAM_BYOB_REQUEST_H_

#include <stdint.h>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-traced-handle.h"
#include "v8/include/v8-typed-array.h"

namespace v8 {
class Isolate;
}  // namespace v8

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class ReadableByteStreamController;

class ReadableStreamBYOBRequest final : public bindings::ScriptWrappable {
 public:
  ReadableStreamBYOBRequest(v8::Isolate* isolate,
                            ReadableByteStreamController* controller,
                            v8::Local<v8::Uint8Array> view);
  ~ReadableStreamBYOBRequest() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface. View() is empty for null.
  v8::Local<v8::ArrayBufferView> View() const;
  void Respond(uint64_t bytes_written,
               bindings::ExceptionState& exception_state);
  void RespondWithNewView(v8::Local<v8::ArrayBufferView> view,
                          bindings::ExceptionState& exception_state);

 private:
  friend class ReadableByteStreamController;

  cppgc::Member<ReadableByteStreamController> controller_;
  // Empty once the request is invalidated.
  v8::TracedReference<v8::ArrayBufferView> view_;
};

}  // namespace mitto

#endif  // MITTO_DOM_READABLE_STREAM_BYOB_REQUEST_H_
