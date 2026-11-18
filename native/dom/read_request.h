// The Streams Standard's read request and read-into request: what a read of a
// default reader, and of a BYOB reader, does with a chunk, the close and an
// error, as Blink's classes.
//
// Upstream: third_party/blink/renderer/core/streams/read_request.h
// Upstream: third_party/blink/renderer/core/streams/read_into_request.h

#ifndef MITTO_DOM_READ_REQUEST_H_
#define MITTO_DOM_READ_REQUEST_H_

#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-typed-array.h"

namespace v8 {
class Isolate;
class Value;
}  // namespace v8

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class ReadRequest : public cppgc::GarbageCollected<ReadRequest> {
 public:
  ReadRequest() = default;
  ReadRequest(const ReadRequest&) = delete;
  ReadRequest& operator=(const ReadRequest&) = delete;
  virtual ~ReadRequest() = default;

  virtual void ChunkSteps(v8::Isolate* isolate,
                          v8::Local<v8::Value> chunk,
                          bindings::ExceptionState& exception_state) const = 0;
  virtual void CloseSteps(v8::Isolate* isolate) const = 0;
  virtual void ErrorSteps(v8::Isolate* isolate,
                          v8::Local<v8::Value> e) const = 0;
  virtual void Trace(cppgc::Visitor* visitor) const {}
};

class ReadIntoRequest : public cppgc::GarbageCollected<ReadIntoRequest> {
 public:
  ReadIntoRequest() = default;
  ReadIntoRequest(const ReadIntoRequest&) = delete;
  ReadIntoRequest& operator=(const ReadIntoRequest&) = delete;
  virtual ~ReadIntoRequest() = default;

  virtual void ChunkSteps(v8::Isolate* isolate,
                          v8::Local<v8::ArrayBufferView> chunk,
                          bindings::ExceptionState& exception_state) const = 0;
  // `chunk` is empty for undefined.
  virtual void CloseSteps(v8::Isolate* isolate,
                          v8::Local<v8::ArrayBufferView> chunk) const = 0;
  virtual void ErrorSteps(v8::Isolate* isolate,
                          v8::Local<v8::Value> e) const = 0;
  virtual void Trace(cppgc::Visitor* visitor) const {}
};

}  // namespace mitto

#endif  // MITTO_DOM_READ_REQUEST_H_
