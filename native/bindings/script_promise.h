// Promises as the Runtime's C++ makes, settles and reacts to them, as Blink's
// ScriptPromise and ScriptPromiseResolver do: a resolver that lives on V8's
// C++ heap and settles its promise at once, and reactions that are
// ScriptFunctions.
//
// Upstream: third_party/blink/renderer/bindings/core/v8/script_promise.h
// Upstream: third_party/blink/renderer/bindings/core/v8/script_promise_resolver.h
// Upstream: third_party/blink/renderer/bindings/core/v8/script_promise_resolver.cc

#ifndef MITTO_BINDINGS_SCRIPT_PROMISE_H_
#define MITTO_BINDINGS_SCRIPT_PROMISE_H_

#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-cppgc.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-traced-handle.h"

namespace v8 {
class Isolate;
class Value;
}  // namespace v8

namespace mitto::bindings {

class ScriptFunction;

// A promise of the current context, and the means to settle it. Settling it
// again does nothing, and its promise stays reachable once it has settled, as
// Blink's resolver keeps it.
class ScriptPromiseResolver final
    : public cppgc::GarbageCollected<ScriptPromiseResolver> {
 public:
  explicit ScriptPromiseResolver(v8::Isolate* isolate);
  ScriptPromiseResolver(const ScriptPromiseResolver&) = delete;
  ScriptPromiseResolver& operator=(const ScriptPromiseResolver&) = delete;
  ~ScriptPromiseResolver();

  v8::Local<v8::Promise> Promise(v8::Isolate* isolate) const;
  // Resolves it with undefined.
  void Resolve(v8::Isolate* isolate);
  void Resolve(v8::Isolate* isolate, v8::Local<v8::Value> value);
  void Reject(v8::Isolate* isolate, v8::Local<v8::Value> reason);
  bool IsSettled() const { return settled_; }

  void Trace(cppgc::Visitor* visitor) const;

 private:
  v8::TracedReference<v8::Promise::Resolver> resolver_;
  bool settled_ = false;
};

// A new promise of the current context, resolved with undefined, as Blink's
// ToResolvedUndefinedPromise() gives.
v8::Local<v8::Promise> ResolvedUndefinedPromise(v8::Isolate* isolate);

// A new promise resolved with `value`, which adopts it if it is a thenable,
// as Blink's ScriptPromise::FromV8Value() does: a promise of the current
// context is the promise itself, as the Promise Resolve abstract operation
// gives it.
v8::Local<v8::Promise> PromiseResolve(v8::Isolate* isolate,
                                      v8::Local<v8::Value> value);

// A new promise rejected with `reason`.
v8::Local<v8::Promise> RejectedPromise(v8::Isolate* isolate,
                                       v8::Local<v8::Value> reason);

// Web IDL's "react to a promise": the promise that `promise`.then() gives,
// with each reaction as a ScriptFunction, or none for one that passes the
// value or reason on. Returns an empty handle if V8 cannot run it.
v8::Local<v8::Promise> PromiseThen(v8::Isolate* isolate,
                                   v8::Local<v8::Promise> promise,
                                   ScriptFunction* on_fulfilled,
                                   ScriptFunction* on_rejected = nullptr);

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_SCRIPT_PROMISE_H_
