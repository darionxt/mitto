// The asynchronous iterator of an interface with an async iterable declaration
// whose class has its behaviour, such as ReadableStream's: an object of the
// iterator's interface, which the bindings make, with an iteration source of
// the class behind it, which gives each result. Its next() and return() run
// Web IDL's steps here, with the ongoing promise that orders them, as Blink's
// AsyncIterationSourceBase does.
//
// Upstream: third_party/blink/renderer/bindings/core/v8/async_iterable.h
// Upstream: third_party/blink/renderer/bindings/core/v8/async_iterable.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/async_iterator_base.h

#ifndef MITTO_BINDINGS_ASYNC_ITERABLE_H_
#define MITTO_BINDINGS_ASYNC_ITERABLE_H_

#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-cppgc.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-traced-handle.h"

namespace v8 {
class Context;
class Isolate;
class Value;
}  // namespace v8

namespace mitto::bindings {

class ScriptFunction;
class ScriptPromiseResolver;
struct WrapperTypeInfo;

// Web IDL's default asynchronous iterator object of a value iterator, without
// its target, which the subclass keeps.
class AsyncIterationSourceBase
    : public cppgc::GarbageCollected<AsyncIterationSourceBase> {
 public:
  AsyncIterationSourceBase(const AsyncIterationSourceBase&) = delete;
  AsyncIterationSourceBase& operator=(const AsyncIterationSourceBase&) =
      delete;
  virtual ~AsyncIterationSourceBase();

  // The iterator's next() and return().
  v8::Local<v8::Promise> Next(v8::Isolate* isolate);
  v8::Local<v8::Promise> Return(v8::Isolate* isolate,
                                v8::Local<v8::Value> value);

  virtual void Trace(cppgc::Visitor* visitor) const;

 protected:
  AsyncIterationSourceBase();

  // Web IDL's "get the next iteration result": settles the pending promise,
  // which TakePendingPromiseResolver() gives, with MakeIterationResult() or
  // MakeEndOfIteration(), or rejects it.
  virtual void GetNextIterationResult() = 0;

  // Web IDL's "asynchronous iterator return": settles the pending promise.
  virtual void AsyncIteratorReturn(v8::Local<v8::Value> value) = 0;

  bool HasPendingPromise() const { return pending_promise_resolver_.Get(); }
  ScriptPromiseResolver* TakePendingPromiseResolver();

  // Web IDL's "end of iteration".
  v8::Local<v8::Value> MakeEndOfIteration(v8::Isolate* isolate) const;
  // The result object of a value.
  v8::Local<v8::Value> MakeIterationResult(v8::Isolate* isolate,
                                           v8::Local<v8::Value> value) const;

 private:
  class RunNextSteps;
  class RunFulfillSteps;
  class RunRejectSteps;
  class RunReturnSteps;
  class RunReturnFulfillSteps;

  v8::Local<v8::Promise> NextSteps(v8::Isolate* isolate);
  v8::Local<v8::Value> FulfillSteps(v8::Isolate* isolate,
                                    v8::Local<v8::Value> next);
  void RejectSteps(v8::Isolate* isolate, v8::Local<v8::Value> reason);
  v8::Local<v8::Promise> ReturnSteps(v8::Isolate* isolate,
                                     v8::Local<v8::Value> value);

  cppgc::Member<ScriptFunction> on_settled_;
  cppgc::Member<ScriptFunction> on_fulfilled_;
  cppgc::Member<ScriptFunction> on_rejected_;
  // Its ongoing promise, which is null when empty, and whether it is finished.
  v8::TracedReference<v8::Promise> ongoing_promise_;
  bool is_finished_ = false;
  // The promise of the next result, or of return(), that the subclass
  // settles.
  cppgc::Member<ScriptPromiseResolver> pending_promise_resolver_;
};

// A new object of the iterator's interface `iterator` with `source` behind it.
v8::MaybeLocal<v8::Value> NewAsyncIterator(v8::Local<v8::Context> context,
                                           const WrapperTypeInfo& iterator,
                                           AsyncIterationSourceBase* source);

// The source behind an asynchronous iterator, or null for one that has none,
// as a Known Gap's iterator has none.
AsyncIterationSourceBase* AsyncIterationSourceOf(v8::Isolate* isolate,
                                                 v8::Local<v8::Value> iterator);

// ECMAScript's CreateIterResultObject(), as Blink's ESCreateIterResultObject()
// makes it: `done`, then `value`.
v8::Local<v8::Object> CreateIterResultObject(v8::Isolate* isolate,
                                             v8::Local<v8::Value> value,
                                             bool done);

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_ASYNC_ITERABLE_H_
