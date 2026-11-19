// The Streams Standard's ReadableStreamDefaultTee(), as Blink's TeeEngine.
//
// Upstream: third_party/blink/renderer/core/streams/tee_engine.h
// Upstream: third_party/blink/renderer/core/streams/tee_engine.cc

#ifndef MITTO_DOM_TEE_ENGINE_H_
#define MITTO_DOM_TEE_ENGINE_H_

#include <array>

#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-maybe.h"
#include "v8/include/v8-traced-handle.h"

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
class ReadableStreamDefaultReader;

class TeeEngine final : public cppgc::GarbageCollected<TeeEngine> {
 public:
  TeeEngine();
  TeeEngine(const TeeEngine&) = delete;
  TeeEngine& operator=(const TeeEngine&) = delete;
  ~TeeEngine();

  // Makes the branches and starts copying.
  void Start(v8::Isolate* isolate,
             ReadableStream* stream,
             bool clone_for_branch2,
             bindings::ExceptionState& exception_state);

  // Null until Start().
  ReadableStream* Branch1() const { return branch_[0].Get(); }
  ReadableStream* Branch2() const { return branch_[1].Get(); }

  void Trace(cppgc::Visitor* visitor) const;

 private:
  class PullAlgorithm;
  class CancelAlgorithm;
  class TeeReadRequest;

  // StructuredClone().
  v8::MaybeLocal<v8::Value> StructuredClone(
      v8::Isolate* isolate,
      v8::Local<v8::Value> chunk,
      bindings::ExceptionState& exception_state);

  cppgc::Member<ReadableStream> stream_;
  cppgc::Member<ReadableStreamDefaultReader> reader_;
  cppgc::Member<bindings::ScriptPromiseResolver> cancel_promise_;
  bool reading_ = false;
  bool read_again_ = false;
  bool clone_for_branch2_ = false;
  // The standard's pairs of variables, one for each branch, from 0: so
  // canceled_[0] is canceled1.
  std::array<bool, 2> canceled_ = {false, false};
  std::array<v8::TracedReference<v8::Value>, 2> reason_;
  std::array<cppgc::Member<ReadableStream>, 2> branch_;
  std::array<cppgc::Member<ReadableStreamDefaultController>, 2> controller_;
};

}  // namespace mitto

#endif  // MITTO_DOM_TEE_ENGINE_H_
