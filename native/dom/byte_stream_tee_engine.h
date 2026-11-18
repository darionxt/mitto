// The Streams Standard's ReadableByteStreamTee(), as Blink's
// ByteStreamTeeEngine.
//
// Upstream: third_party/blink/renderer/core/streams/byte_stream_tee_engine.h
// Upstream: third_party/blink/renderer/core/streams/byte_stream_tee_engine.cc

#ifndef MITTO_DOM_BYTE_STREAM_TEE_ENGINE_H_
#define MITTO_DOM_BYTE_STREAM_TEE_ENGINE_H_

#include <array>

#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-traced-handle.h"
#include "v8/include/v8-typed-array.h"

namespace v8 {
class Isolate;
class Value;
}  // namespace v8

namespace mitto {

namespace bindings {
class ExceptionState;
class ScriptPromiseResolver;
}  // namespace bindings

class ReadableByteStreamController;
class ReadableStream;
class ReadableStreamGenericReader;

class ByteStreamTeeEngine final
    : public cppgc::GarbageCollected<ByteStreamTeeEngine> {
 public:
  ByteStreamTeeEngine();
  ByteStreamTeeEngine(const ByteStreamTeeEngine&) = delete;
  ByteStreamTeeEngine& operator=(const ByteStreamTeeEngine&) = delete;
  ~ByteStreamTeeEngine();

  // Makes the branches and starts copying.
  void Start(v8::Isolate* isolate,
             ReadableStream* stream,
             bindings::ExceptionState& exception_state);

  // Null until Start().
  ReadableStream* Branch1() const { return branch_[0].Get(); }
  ReadableStream* Branch2() const { return branch_[1].Get(); }

  void Trace(cppgc::Visitor* visitor) const;

 private:
  class PullAlgorithm;
  class CancelAlgorithm;
  class ByteTeeReadRequest;
  class ByteTeeReadIntoRequest;

  void ForwardReaderError(v8::Isolate* isolate,
                          ReadableStreamGenericReader* this_reader);
  void PullWithDefaultReader(v8::Isolate* isolate,
                             bindings::ExceptionState& exception_state);
  void PullWithBYOBReader(v8::Isolate* isolate,
                          v8::Local<v8::ArrayBufferView> view,
                          bool for_branch_2,
                          bindings::ExceptionState& exception_state);
  // CloneAsUint8Array().
  v8::Local<v8::Uint8Array> CloneAsUint8Array(
      v8::Isolate* isolate,
      v8::Local<v8::ArrayBufferView> chunk);

  cppgc::Member<ReadableStream> stream_;
  cppgc::Member<ReadableStreamGenericReader> reader_;
  cppgc::Member<bindings::ScriptPromiseResolver> cancel_promise_;
  bool reading_ = false;
  // The standard's pairs of variables, one for each branch, from 0: so
  // canceled_[0] is canceled1.
  std::array<bool, 2> canceled_ = {false, false};
  std::array<bool, 2> read_again_for_branch_ = {false, false};
  std::array<v8::TracedReference<v8::Value>, 2> reason_;
  std::array<cppgc::Member<ReadableStream>, 2> branch_;
  std::array<cppgc::Member<ReadableByteStreamController>, 2> controller_;
};

}  // namespace mitto

#endif  // MITTO_DOM_BYTE_STREAM_TEE_ENGINE_H_
