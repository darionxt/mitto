// A function that the Runtime's C++ hands to V8, such as a reaction to a
// promise, whose behaviour is a C++ object on V8's C++ heap, as Blink's
// ScriptFunction is. Its JavaScript function holds the object, so that it
// lives as long as V8 can call it.
//
// Upstream: third_party/blink/renderer/bindings/core/v8/script_function.h
// Upstream: third_party/blink/renderer/bindings/core/v8/script_function.cc

#ifndef MITTO_BINDINGS_SCRIPT_FUNCTION_H_
#define MITTO_BINDINGS_SCRIPT_FUNCTION_H_

#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-cppgc.h"
#include "v8/include/v8-function-callback.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-traced-handle.h"

namespace v8 {
class Function;
class Isolate;
class Value;
}  // namespace v8

namespace mitto::bindings {

class ScriptFunction : public cppgc::GarbageCollected<ScriptFunction> {
 public:
  ScriptFunction();
  ScriptFunction(const ScriptFunction&) = delete;
  ScriptFunction& operator=(const ScriptFunction&) = delete;
  virtual ~ScriptFunction();

  // What a call does with its first argument, and what it returns. A subclass
  // implements this or CallRaw().
  virtual v8::Local<v8::Value> Call(v8::Isolate* isolate,
                                    v8::Local<v8::Value> value);

  // What a call does, for one that needs all of the call. By default, it
  // returns what Call() returns for the first argument.
  virtual void CallRaw(const v8::FunctionCallbackInfo<v8::Value>& info);

  // The function's `length`, 0 by default.
  virtual int Length() const;

  virtual void Trace(cppgc::Visitor* visitor) const;

  // The JavaScript function, which this makes in the current context the
  // first time.
  v8::Local<v8::Function> ToV8Function(v8::Isolate* isolate);

 private:
  v8::TracedReference<v8::Function> function_;
};

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_SCRIPT_FUNCTION_H_
