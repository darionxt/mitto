#include "mitto/bindings/script_promise.h"

#include <tuple>

#include "base/check.h"
#include "mitto/bindings/script_function.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-primitive.h"

namespace mitto::bindings {

ScriptPromiseResolver::ScriptPromiseResolver(v8::Isolate* isolate)
    : resolver_(isolate,
                v8::Promise::Resolver::New(isolate->GetCurrentContext())
                    .ToLocalChecked()) {}

ScriptPromiseResolver::~ScriptPromiseResolver() = default;

v8::Local<v8::Promise> ScriptPromiseResolver::Promise(
    v8::Isolate* isolate) const {
  return resolver_.Get(isolate)->GetPromise();
}

void ScriptPromiseResolver::Resolve(v8::Isolate* isolate) {
  Resolve(isolate, v8::Undefined(isolate));
}

void ScriptPromiseResolver::Resolve(v8::Isolate* isolate,
                                    v8::Local<v8::Value> value) {
  if (settled_) {
    return;
  }
  settled_ = true;
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::MicrotasksScope microtasks_scope(
      context, v8::MicrotasksScope::kDoNotRunMicrotasks);
  std::ignore = resolver_.Get(isolate)->Resolve(context, value);
}

void ScriptPromiseResolver::Reject(v8::Isolate* isolate,
                                   v8::Local<v8::Value> reason) {
  if (settled_) {
    return;
  }
  settled_ = true;
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::MicrotasksScope microtasks_scope(
      context, v8::MicrotasksScope::kDoNotRunMicrotasks);
  std::ignore = resolver_.Get(isolate)->Reject(context, reason);
}

void ScriptPromiseResolver::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(resolver_);
}

v8::Local<v8::Promise> ResolvedUndefinedPromise(v8::Isolate* isolate) {
  return PromiseResolve(isolate, v8::Undefined(isolate));
}

v8::Local<v8::Promise> PromiseResolve(v8::Isolate* isolate,
                                      v8::Local<v8::Value> value) {
  CHECK(!value.IsEmpty());
  if (value->IsPromise()) {
    return value.As<v8::Promise>();
  }
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::MicrotasksScope microtasks_scope(
      context, v8::MicrotasksScope::kDoNotRunMicrotasks);
  const v8::Local<v8::Promise::Resolver> resolver =
      v8::Promise::Resolver::New(context).ToLocalChecked();
  std::ignore = resolver->Resolve(context, value);
  return resolver->GetPromise();
}

v8::Local<v8::Promise> RejectedPromise(v8::Isolate* isolate,
                                       v8::Local<v8::Value> reason) {
  CHECK(!reason.IsEmpty());
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::MicrotasksScope microtasks_scope(
      context, v8::MicrotasksScope::kDoNotRunMicrotasks);
  const v8::Local<v8::Promise::Resolver> resolver =
      v8::Promise::Resolver::New(context).ToLocalChecked();
  std::ignore = resolver->Reject(context, reason);
  return resolver->GetPromise();
}

v8::Local<v8::Promise> PromiseThen(v8::Isolate* isolate,
                                   v8::Local<v8::Promise> promise,
                                   ScriptFunction* on_fulfilled,
                                   ScriptFunction* on_rejected) {
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Promise> result;
  if (on_fulfilled && on_rejected) {
    std::ignore =
        promise
            ->Then(context, on_fulfilled->ToV8Function(isolate),
                   on_rejected->ToV8Function(isolate))
            .ToLocal(&result);
  } else if (on_fulfilled) {
    std::ignore = promise->Then(context, on_fulfilled->ToV8Function(isolate))
                      .ToLocal(&result);
  } else if (on_rejected) {
    std::ignore = promise->Catch(context, on_rejected->ToV8Function(isolate))
                      .ToLocal(&result);
  } else {
    result = promise;
  }
  return result;
}

}  // namespace mitto::bindings
