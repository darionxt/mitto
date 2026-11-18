#include "mitto/bindings/async_iterable.h"

#include <tuple>

#include "base/check.h"
#include "mitto/bindings/known_gap.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/script_function.h"
#include "mitto/bindings/script_promise.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/wrapper_type_info.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"

namespace mitto::bindings {

namespace {

// The C++ side of an asynchronous iterator, which holds its source, as
// Blink's AsyncIteratorBase does.
class AsyncIterator final : public v8::Object::Wrappable {
 public:
  explicit AsyncIterator(AsyncIterationSourceBase* source) : source_(source) {}

  const char* GetHumanReadableName() const override { return "AsyncIterator"; }

  void Trace(cppgc::Visitor* visitor) const override {
    v8::Object::Wrappable::Trace(visitor);
    visitor->Trace(source_);
  }

  AsyncIterationSourceBase* source() const { return source_.Get(); }

 private:
  const cppgc::Member<AsyncIterationSourceBase> source_;
};

}  // namespace

// What each step's function calls on the source.
class AsyncIterationSourceBase::RunNextSteps final : public ScriptFunction {
 public:
  explicit RunNextSteps(AsyncIterationSourceBase* source) : source_(source) {}
  v8::Local<v8::Value> Call(v8::Isolate* isolate,
                            v8::Local<v8::Value>) override {
    return source_->NextSteps(isolate);
  }
  void Trace(cppgc::Visitor* visitor) const override {
    ScriptFunction::Trace(visitor);
    visitor->Trace(source_);
  }

 private:
  const cppgc::Member<AsyncIterationSourceBase> source_;
};

class AsyncIterationSourceBase::RunFulfillSteps final : public ScriptFunction {
 public:
  explicit RunFulfillSteps(AsyncIterationSourceBase* source)
      : source_(source) {}
  v8::Local<v8::Value> Call(v8::Isolate* isolate,
                            v8::Local<v8::Value> next) override {
    return source_->FulfillSteps(isolate, next);
  }
  void Trace(cppgc::Visitor* visitor) const override {
    ScriptFunction::Trace(visitor);
    visitor->Trace(source_);
  }

 private:
  const cppgc::Member<AsyncIterationSourceBase> source_;
};

class AsyncIterationSourceBase::RunRejectSteps final : public ScriptFunction {
 public:
  explicit RunRejectSteps(AsyncIterationSourceBase* source) : source_(source) {}
  v8::Local<v8::Value> Call(v8::Isolate* isolate,
                            v8::Local<v8::Value> reason) override {
    source_->RejectSteps(isolate, reason);
    return {};
  }
  void Trace(cppgc::Visitor* visitor) const override {
    ScriptFunction::Trace(visitor);
    visitor->Trace(source_);
  }

 private:
  const cppgc::Member<AsyncIterationSourceBase> source_;
};

class AsyncIterationSourceBase::RunReturnSteps final : public ScriptFunction {
 public:
  RunReturnSteps(v8::Isolate* isolate,
                 AsyncIterationSourceBase* source,
                 v8::Local<v8::Value> value)
      : source_(source), value_(isolate, value) {}
  v8::Local<v8::Value> Call(v8::Isolate* isolate,
                            v8::Local<v8::Value>) override {
    return source_->ReturnSteps(isolate, value_.Get(isolate));
  }
  void Trace(cppgc::Visitor* visitor) const override {
    ScriptFunction::Trace(visitor);
    visitor->Trace(source_);
    visitor->Trace(value_);
  }

 private:
  const cppgc::Member<AsyncIterationSourceBase> source_;
  const v8::TracedReference<v8::Value> value_;
};

class AsyncIterationSourceBase::RunReturnFulfillSteps final
    : public ScriptFunction {
 public:
  RunReturnFulfillSteps(v8::Isolate* isolate, v8::Local<v8::Value> value)
      : value_(isolate, value) {}
  v8::Local<v8::Value> Call(v8::Isolate* isolate,
                            v8::Local<v8::Value>) override {
    // Step 13.1. Return CreateIterResultObject(value, true).
    return CreateIterResultObject(isolate, value_.Get(isolate), true);
  }
  void Trace(cppgc::Visitor* visitor) const override {
    ScriptFunction::Trace(visitor);
    visitor->Trace(value_);
  }

 private:
  const v8::TracedReference<v8::Value> value_;
};

AsyncIterationSourceBase::AsyncIterationSourceBase()
    : on_settled_(MakeGarbageCollected<RunNextSteps>(this)),
      on_fulfilled_(MakeGarbageCollected<RunFulfillSteps>(this)),
      on_rejected_(MakeGarbageCollected<RunRejectSteps>(this)) {}

AsyncIterationSourceBase::~AsyncIterationSourceBase() = default;

v8::Local<v8::Promise> AsyncIterationSourceBase::Next(v8::Isolate* isolate) {
  v8::Local<v8::Promise> next_promise;
  if (!ongoing_promise_.IsEmpty()) {
    // Step 10. If ongoingPromise is not null, then: perform
    // PerformPromiseThen(ongoingPromise, onSettled, onSettled,
    // afterOngoingPromiseCapability).
    next_promise = PromiseThen(isolate, ongoing_promise_.Get(isolate),
                               on_settled_.Get(), on_settled_.Get());
  } else {
    // Step 11. Otherwise: set the ongoing promise to the result of running
    // nextSteps.
    next_promise = NextSteps(isolate);
  }
  if (next_promise.IsEmpty()) {
    return next_promise;
  }
  ongoing_promise_.Reset(isolate, next_promise);
  // Step 12. Return object's ongoing promise.
  return next_promise;
}

v8::Local<v8::Promise> AsyncIterationSourceBase::Return(
    v8::Isolate* isolate,
    v8::Local<v8::Value> value) {
  v8::Local<v8::Promise> next_promise;
  if (!ongoing_promise_.IsEmpty()) {
    // Step 10. If ongoingPromise is not null: PerformPromiseThen(
    // ongoingPromise, onSettled, onSettled, afterOngoingPromiseCapability),
    // with onSettled running returnSteps.
    ScriptFunction* const on_settled =
        MakeGarbageCollected<RunReturnSteps>(isolate, this, value);
    next_promise = PromiseThen(isolate, ongoing_promise_.Get(isolate),
                               on_settled, on_settled);
  } else {
    // Step 11. Otherwise: the result of running returnSteps.
    next_promise = ReturnSteps(isolate, value);
  }
  if (next_promise.IsEmpty()) {
    return next_promise;
  }
  ongoing_promise_.Reset(isolate, next_promise);
  // Steps 13-15: PerformPromiseThen(object's ongoing promise, onFulfilled,
  // undefined, returnPromiseCapability).
  return PromiseThen(
      isolate, next_promise,
      MakeGarbageCollected<RunReturnFulfillSteps>(isolate, value));
}

void AsyncIterationSourceBase::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(on_settled_);
  visitor->Trace(on_fulfilled_);
  visitor->Trace(on_rejected_);
  visitor->Trace(ongoing_promise_);
  visitor->Trace(pending_promise_resolver_);
}

ScriptPromiseResolver* AsyncIterationSourceBase::TakePendingPromiseResolver() {
  CHECK(pending_promise_resolver_);
  ScriptPromiseResolver* const resolver = pending_promise_resolver_.Get();
  pending_promise_resolver_ = nullptr;
  return resolver;
}

v8::Local<v8::Value> AsyncIterationSourceBase::MakeEndOfIteration(
    v8::Isolate* isolate) const {
  // Undefined stands for the special "end of iteration" value.
  return v8::Undefined(isolate);
}

v8::Local<v8::Value> AsyncIterationSourceBase::MakeIterationResult(
    v8::Isolate* isolate,
    v8::Local<v8::Value> value) const {
  return CreateIterResultObject(isolate, value, false);
}

// Step 8. Let nextSteps be the following steps.
v8::Local<v8::Promise> AsyncIterationSourceBase::NextSteps(
    v8::Isolate* isolate) {
  if (is_finished_) {
    // Step 8.2. If object's is finished is true, resolve with
    // CreateIterResultObject(undefined, true).
    return PromiseResolve(isolate, CreateIterResultObject(
                                       isolate, v8::Undefined(isolate), true));
  }
  // Steps 8.4-8.10: get the next iteration result, and react to it with
  // fulfillSteps and rejectSteps.
  CHECK(!pending_promise_resolver_);
  pending_promise_resolver_ = MakeGarbageCollected<ScriptPromiseResolver>(isolate);
  const v8::Local<v8::Promise> promise =
      pending_promise_resolver_->Promise(isolate);
  GetNextIterationResult();
  return PromiseThen(isolate, promise, on_fulfilled_.Get(),
                     on_rejected_.Get());
}

// Step 8.5. Let fulfillSteps be the following steps, given next, which is an
// iterator result object or undefined for the end of iteration.
v8::Local<v8::Value> AsyncIterationSourceBase::FulfillSteps(
    v8::Isolate* isolate,
    v8::Local<v8::Value> next) {
  // Step 8.5.1. Set object's ongoing promise to null.
  ongoing_promise_.Reset();
  // Step 8.5.2. If next is end of iteration, then set object's is finished to
  // true, and return CreateIterResultObject(undefined, true).
  if (next->IsUndefined()) {
    is_finished_ = true;
    return CreateIterResultObject(isolate, v8::Undefined(isolate), true);
  }
  // Step 8.5.4. Otherwise, next is already the result object.
  return next;
}

// Step 8.7. Let rejectSteps be the following steps, given reason.
void AsyncIterationSourceBase::RejectSteps(v8::Isolate* isolate,
                                           v8::Local<v8::Value> reason) {
  // Steps 8.7.1-8.7.3: set the ongoing promise to null, is finished to true,
  // and throw reason.
  ongoing_promise_.Reset();
  is_finished_ = true;
  isolate->ThrowException(reason);
}

// Step 8 of the return() steps. Let returnSteps be the following steps.
v8::Local<v8::Promise> AsyncIterationSourceBase::ReturnSteps(
    v8::Isolate* isolate,
    v8::Local<v8::Value> value) {
  if (is_finished_) {
    // Step 8.2. If object's is finished is true, resolve with
    // CreateIterResultObject(value, true).
    return PromiseResolve(isolate, CreateIterResultObject(isolate, value, true));
  }
  // Step 8.3. Set object's is finished to true.
  is_finished_ = true;
  // Step 8.4. Return the result of running the asynchronous iterator return
  // algorithm.
  CHECK(!pending_promise_resolver_);
  pending_promise_resolver_ = MakeGarbageCollected<ScriptPromiseResolver>(isolate);
  const v8::Local<v8::Promise> promise =
      pending_promise_resolver_->Promise(isolate);
  AsyncIteratorReturn(value);
  return promise;
}

v8::MaybeLocal<v8::Value> NewAsyncIterator(v8::Local<v8::Context> context,
                                           const WrapperTypeInfo& iterator,
                                           AsyncIterationSourceBase* source) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::Value> object;
  if (!NewObjectOfInterface(context, iterator).ToLocal(&object)) {
    return {};
  }
  v8::Object::Wrap<kAsyncIteratorTag>(
      isolate, object.As<v8::Object>(),
      MakeGarbageCollected<AsyncIterator>(source));
  return object;
}

AsyncIterationSourceBase* AsyncIterationSourceOf(
    v8::Isolate* isolate,
    v8::Local<v8::Value> iterator) {
  if (!iterator->IsObject()) {
    return nullptr;
  }
  AsyncIterator* const wrappable =
      v8::Object::Unwrap<kAsyncIteratorTag, AsyncIterator>(
          isolate, iterator.As<v8::Object>());
  return wrappable ? wrappable->source() : nullptr;
}

v8::Local<v8::Object> CreateIterResultObject(v8::Isolate* isolate,
                                             v8::Local<v8::Value> value,
                                             bool done) {
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::Local<v8::Object> result = v8::Object::New(isolate);
  std::ignore = result->CreateDataProperty(
      context, v8::String::NewFromUtf8Literal(isolate, "done"),
      v8::Boolean::New(isolate, done));
  std::ignore = result->CreateDataProperty(
      context, v8::String::NewFromUtf8Literal(isolate, "value"), value);
  return result;
}

}  // namespace mitto::bindings
