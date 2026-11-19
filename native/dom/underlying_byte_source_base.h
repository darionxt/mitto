// What the Runtime's own byte source gives a byte stream, such as a response's
// body: its pull and its cancel, as Blink's UnderlyingByteSourceBase, after
// the Streams Standard's underlying source. It signals an error by throwing or
// by rejecting.
//
// Upstream: third_party/blink/renderer/core/streams/underlying_byte_source_base.h

#ifndef MITTO_DOM_UNDERLYING_BYTE_SOURCE_BASE_H_
#define MITTO_DOM_UNDERLYING_BYTE_SOURCE_BASE_H_

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

class ReadableByteStreamController;

class UnderlyingByteSourceBase
    : public cppgc::GarbageCollected<UnderlyingByteSourceBase> {
 public:
  UnderlyingByteSourceBase() = default;
  UnderlyingByteSourceBase(const UnderlyingByteSourceBase&) = delete;
  UnderlyingByteSourceBase& operator=(const UnderlyingByteSourceBase&) = delete;
  virtual ~UnderlyingByteSourceBase() = default;

  virtual v8::Local<v8::Promise> Pull(
      ReadableByteStreamController* controller,
      bindings::ExceptionState& exception_state) = 0;
  virtual v8::Local<v8::Promise> Cancel(v8::Local<v8::Value> reason) = 0;

  virtual void Trace(cppgc::Visitor* visitor) const {}
};

}  // namespace mitto

#endif  // MITTO_DOM_UNDERLYING_BYTE_SOURCE_BASE_H_
