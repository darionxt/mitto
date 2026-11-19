#include "mitto/dom/miscellaneous_operations.h"

#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/containers/span.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/strcat.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/script_promise.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/dom/stream_algorithms.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-traced-handle.h"

namespace mitto {

namespace {

v8::Local<v8::String> NameString(v8::Isolate* isolate, const char* name) {
  return v8::String::NewFromUtf8(isolate, name, v8::NewStringType::kInternalized)
      .ToLocalChecked();
}

class DefaultSizeAlgorithm final : public StrategySizeAlgorithm {
 public:
  std::optional<double> Run(v8::Isolate*, v8::Local<v8::Value>) override {
    return 1;
  }
};

class JavaScriptSizeAlgorithm final : public StrategySizeAlgorithm {
 public:
  JavaScriptSizeAlgorithm(v8::Isolate* isolate, v8::Local<v8::Function> size)
      : function_(isolate, size) {}

  std::optional<double> Run(v8::Isolate* isolate,
                            v8::Local<v8::Value> chunk) override {
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();
    v8::Local<v8::Value> argv[] = {chunk};
    // MakeSizeAlgorithmFromSizeFunction() step 3.a. Return ? Call(size,
    // undefined, « chunk »).
    v8::Local<v8::Value> result;
    if (!function_.Get(isolate)
             ->Call(context, v8::Undefined(isolate), 1, argv)
             .ToLocal(&result)) {
      return std::nullopt;
    }
    // EnqueueValueWithSize() step 2. Let size be ? ToNumber(size).
    v8::Local<v8::Number> number;
    if (!result->ToNumber(context).ToLocal(&number)) {
      return std::nullopt;
    }
    return number->Value();
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(function_);
    StrategySizeAlgorithm::Trace(visitor);
  }

 private:
  v8::TracedReference<v8::Function> function_;
};

class TrivialStreamAlgorithm final : public StreamAlgorithm {
 public:
  v8::Local<v8::Promise> Run(v8::Isolate* isolate,
                             base::span<v8::Local<v8::Value>>) override {
    return bindings::ResolvedUndefinedPromise(isolate);
  }
};

class JavaScriptStreamAlgorithmWithoutExtraArg final : public StreamAlgorithm {
 public:
  JavaScriptStreamAlgorithmWithoutExtraArg(v8::Isolate* isolate,
                                           v8::Local<v8::Function> method,
                                           v8::Local<v8::Object> recv)
      : recv_(isolate, recv), method_(isolate, method) {}

  v8::Local<v8::Promise> Run(
      v8::Isolate* isolate,
      base::span<v8::Local<v8::Value>> arguments) override {
    // CreateAlgorithmFromUnderlyingMethod() step 6.b.i. Return !
    // PromiseCall(method, underlyingObject, extraArgs), with the arguments
    // that the algorithm takes.
    return PromiseCall(isolate, method_.Get(isolate), recv_.Get(isolate),
                       base::checked_cast<int>(arguments.size()),
                       arguments.data());
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(recv_);
    visitor->Trace(method_);
    StreamAlgorithm::Trace(visitor);
  }

 private:
  v8::TracedReference<v8::Object> recv_;
  v8::TracedReference<v8::Function> method_;
};

class JavaScriptStreamAlgorithmWithExtraArg final : public StreamAlgorithm {
 public:
  JavaScriptStreamAlgorithmWithExtraArg(v8::Isolate* isolate,
                                        v8::Local<v8::Function> method,
                                        v8::Local<v8::Value> extra_arg,
                                        v8::Local<v8::Object> recv)
      : recv_(isolate, recv),
        method_(isolate, method),
        extra_arg_(isolate, extra_arg) {}

  v8::Local<v8::Promise> Run(
      v8::Isolate* isolate,
      base::span<v8::Local<v8::Value>> arguments) override {
    CHECK_LE(arguments.size(), 1u);
    // CreateAlgorithmFromUnderlyingMethod() step 6.c: fullArgs is arg followed
    // by extraArgs, and PromiseCall(method, underlyingObject, fullArgs).
    std::array<v8::Local<v8::Value>, 2> full_argv;
    if (!arguments.empty()) {
      full_argv[0] = arguments[0];
    }
    full_argv[arguments.size()] = extra_arg_.Get(isolate);
    const int full_argc = base::checked_cast<int>(arguments.size() + 1);
    return PromiseCall(isolate, method_.Get(isolate), recv_.Get(isolate),
                       full_argc, full_argv.data());
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(recv_);
    visitor->Trace(method_);
    visitor->Trace(extra_arg_);
    StreamAlgorithm::Trace(visitor);
  }

 private:
  v8::TracedReference<v8::Object> recv_;
  v8::TracedReference<v8::Function> method_;
  v8::TracedReference<v8::Value> extra_arg_;
};

class JavaScriptByteStreamStartAlgorithm final : public StreamStartAlgorithm {
 public:
  JavaScriptByteStreamStartAlgorithm(v8::Isolate* isolate,
                                     v8::Local<v8::Function> method,
                                     v8::Local<v8::Object> recv,
                                     v8::Local<v8::Value> controller)
      : recv_(isolate, recv),
        method_(isolate, method),
        controller_(isolate, controller) {}

  v8::Local<v8::Promise> Run(v8::Isolate* isolate) override {
    v8::Local<v8::Value> controller = controller_.Get(isolate);
    v8::Local<v8::Value> value;
    if (!method_.Get(isolate)
             ->Call(isolate->GetCurrentContext(), recv_.Get(isolate), 1,
                    &controller)
             .ToLocal(&value)) {
      return {};
    }
    return bindings::PromiseResolve(isolate, value);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(recv_);
    visitor->Trace(method_);
    visitor->Trace(controller_);
    StreamStartAlgorithm::Trace(visitor);
  }

 private:
  v8::TracedReference<v8::Object> recv_;
  v8::TracedReference<v8::Function> method_;
  v8::TracedReference<v8::Value> controller_;
};

class JavaScriptStreamStartAlgorithm final : public StreamStartAlgorithm {
 public:
  JavaScriptStreamStartAlgorithm(v8::Isolate* isolate,
                                 v8::Local<v8::Object> recv,
                                 const char* method_name_for_error,
                                 v8::Local<v8::Value> controller)
      : recv_(isolate, recv),
        method_name_for_error_(method_name_for_error),
        controller_(isolate, controller) {}

  v8::Local<v8::Promise> Run(v8::Isolate* isolate) override {
    // SetUpWritableStreamDefaultControllerFromUnderlyingSink() step 3: the
    // start algorithm returns ? InvokeOrNoop(underlyingSink, "start",
    // « controller »).
    bindings::ExceptionState exception_state(isolate);
    v8::Local<v8::Value> value;
    if (!CallOrNoop1(isolate, recv_.Get(isolate), "start",
                     method_name_for_error_, controller_.Get(isolate),
                     exception_state)
             .ToLocal(&value)) {
      return {};
    }
    return bindings::PromiseResolve(isolate, value);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(recv_);
    visitor->Trace(controller_);
    StreamStartAlgorithm::Trace(visitor);
  }

 private:
  v8::TracedReference<v8::Object> recv_;
  const char* const method_name_for_error_;
  v8::TracedReference<v8::Value> controller_;
};

// A task that EnqueueMicrotask() queued, with the context to run it in.
struct QueuedMicrotask {
  base::OnceClosure task;
  v8::Global<v8::Context> context;
};

void RunQueuedMicrotask(void* data) {
  const std::unique_ptr<QueuedMicrotask> queued(
      static_cast<QueuedMicrotask*>(data));
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::HandleScope handle_scope(isolate);
  const v8::Context::Scope context_scope(queued->context.Get(isolate));
  std::move(queued->task).Run();
}

class TrivialStartAlgorithm final : public StreamStartAlgorithm {
 public:
  v8::Local<v8::Promise> Run(v8::Isolate* isolate) override {
    return bindings::ResolvedUndefinedPromise(isolate);
  }
};

}  // namespace

StreamAlgorithm* CreateAlgorithmFromUnderlyingMethod(
    v8::Isolate* isolate,
    v8::Local<v8::Object> underlying_object,
    const char* method_name,
    const char* method_name_for_error,
    v8::MaybeLocal<v8::Value> extra_arg,
    bindings::ExceptionState& exception_state) {
  // Steps 5-6. Let method be ? GetV(underlyingObject, methodName), and throw a
  // TypeError if it is not undefined and not callable.
  v8::Local<v8::Value> method;
  if (!ResolveMethod(isolate, underlying_object, method_name,
                     method_name_for_error, exception_state)
           .ToLocal(&method)) {
    CHECK(exception_state.HadException());
    return nullptr;
  }
  if (method->IsUndefined()) {
    // Step 7. Return an algorithm which returns a promise resolved with
    // undefined.
    return bindings::MakeGarbageCollected<TrivialStreamAlgorithm>();
  }
  return CreateAlgorithmFromResolvedMethod(isolate, underlying_object, method,
                                           extra_arg);
}

v8::MaybeLocal<v8::Value> ResolveMethod(
    v8::Isolate* isolate,
    v8::Local<v8::Object> object,
    const char* method_name,
    const char* name_for_error,
    bindings::ExceptionState& exception_state) {
  v8::Local<v8::Value> method;
  {
    const bindings::TryRethrowScope rethrow_scope(isolate, exception_state);
    // Step 5. Let method be ? GetV(underlyingObject, methodName).
    if (!object->Get(isolate->GetCurrentContext(), NameString(isolate, method_name))
             .ToLocal(&method)) {
      return {};
    }
  }
  // Step 6.a. If ! IsCallable(method) is false, throw a TypeError exception.
  if (!method->IsFunction() && !method->IsUndefined()) {
    exception_state.ThrowTypeError(
        base::StrCat({name_for_error, " must be a function or undefined"}));
    return {};
  }
  return method;
}

StreamAlgorithm* CreateAlgorithmFromResolvedMethod(
    v8::Isolate* isolate,
    v8::Local<v8::Object> underlying_object,
    v8::Local<v8::Value> method,
    v8::MaybeLocal<v8::Value> extra_arg) {
  CHECK(method->IsFunction());
  // The standard switches on how many arguments the algorithm takes; this
  // switches on whether there is an extra argument, which decides whether the
  // arguments are put together when it runs.
  v8::Local<v8::Value> extra_arg_local;
  if (!extra_arg.ToLocal(&extra_arg_local)) {
    return bindings::MakeGarbageCollected<
        JavaScriptStreamAlgorithmWithoutExtraArg>(
        isolate, method.As<v8::Function>(), underlying_object);
  }
  return bindings::MakeGarbageCollected<JavaScriptStreamAlgorithmWithExtraArg>(
      isolate, method.As<v8::Function>(), extra_arg_local, underlying_object);
}

StreamStartAlgorithm* CreateStartAlgorithm(
    v8::Isolate* isolate,
    v8::Local<v8::Object> underlying_object,
    const char* method_name_for_error,
    v8::Local<v8::Value> controller) {
  return bindings::MakeGarbageCollected<JavaScriptStreamStartAlgorithm>(
      isolate, underlying_object, method_name_for_error, controller);
}

StreamStartAlgorithm* CreateByteStreamStartAlgorithm(
    v8::Isolate* isolate,
    v8::Local<v8::Object> underlying_object,
    v8::Local<v8::Value> method,
    v8::Local<v8::Value> controller) {
  return bindings::MakeGarbageCollected<JavaScriptByteStreamStartAlgorithm>(
      isolate, method.As<v8::Function>(), underlying_object, controller);
}

StreamStartAlgorithm* CreateTrivialStartAlgorithm() {
  return bindings::MakeGarbageCollected<TrivialStartAlgorithm>();
}

StreamAlgorithm* CreateTrivialStreamAlgorithm() {
  return bindings::MakeGarbageCollected<TrivialStreamAlgorithm>();
}

v8::Local<v8::Object> CreateTrivialQueuingStrategy(v8::Isolate* isolate,
                                                   size_t high_water_mark) {
  v8::Local<v8::Name> high_water_mark_string =
      NameString(isolate, "highWaterMark");
  v8::Local<v8::Value> high_water_mark_value =
      v8::Number::New(isolate, static_cast<double>(high_water_mark));
  return v8::Object::New(isolate, v8::Null(isolate), &high_water_mark_string,
                         &high_water_mark_value, 1);
}

v8::MaybeLocal<v8::Value> CallOrNoop1(
    v8::Isolate* isolate,
    v8::Local<v8::Object> object,
    const char* method_name,
    const char* name_for_error,
    v8::Local<v8::Value> arg0,
    bindings::ExceptionState& exception_state) {
  // InvokeOrNoop() step 4. Let method be ? GetV(O, P).
  v8::Local<v8::Value> method;
  if (!ResolveMethod(isolate, object, method_name, name_for_error,
                     exception_state)
           .ToLocal(&method)) {
    CHECK(exception_state.HadException());
    return {};
  }
  // Step 5. If method is undefined, return undefined.
  if (method->IsUndefined()) {
    return v8::Undefined(isolate);
  }
  CHECK(method->IsFunction());
  // Step 6. Return ? Call(method, O, args).
  const bindings::TryRethrowScope rethrow_scope(isolate, exception_state);
  return method.As<v8::Function>()->Call(isolate->GetCurrentContext(), object,
                                         1, &arg0);
}

v8::Local<v8::Promise> PromiseCall(v8::Isolate* isolate,
                                   v8::Local<v8::Function> method,
                                   v8::Local<v8::Object> recv,
                                   int argc,
                                   v8::Local<v8::Value> argv[]) {
  CHECK_GE(argc, 0);
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::TryCatch try_catch(isolate);
  const v8::MicrotasksScope microtasks_scope(
      context, v8::MicrotasksScope::kDoNotRunMicrotasks);
  // Step 4. Let returnValue be Call(F, V, args).
  v8::Local<v8::Value> result;
  if (!method->Call(context, recv, argc, argv).ToLocal(&result)) {
    // Step 5. If returnValue is an abrupt completion, return a promise
    // rejected with returnValue.[[Value]].
    return bindings::RejectedPromise(isolate, try_catch.Exception());
  }
  // Step 6. Otherwise, return a promise resolved with returnValue.[[Value]].
  return bindings::PromiseResolve(isolate, result);
}

double ValidateAndNormalizeHighWaterMark(
    double high_water_mark,
    bindings::ExceptionState& exception_state) {
  // Step 2. If highWaterMark is NaN or highWaterMark < 0, throw a RangeError
  // exception.
  if (std::isnan(high_water_mark) || high_water_mark < 0) {
    exception_state.ThrowRangeError(
        "A queuing strategy's highWaterMark property must be a nonnegative, "
        "non-NaN number");
    return 0;
  }
  // Step 3. Return highWaterMark.
  return high_water_mark;
}

StrategySizeAlgorithm* MakeSizeAlgorithmFromSizeFunction(
    v8::Isolate* isolate,
    v8::Local<v8::Value> size,
    bindings::ExceptionState& exception_state) {
  // Step 1. If size is undefined, return an algorithm that returns 1.
  if (size->IsUndefined()) {
    return bindings::MakeGarbageCollected<DefaultSizeAlgorithm>();
  }
  // Step 2. If ! IsCallable(size) is false, throw a TypeError exception.
  if (!size->IsFunction()) {
    exception_state.ThrowTypeError(
        "A queuing strategy's size property must be a function");
    return nullptr;
  }
  // Step 3. Return an algorithm that calls size with the chunk.
  return bindings::MakeGarbageCollected<JavaScriptSizeAlgorithm>(
      isolate, size.As<v8::Function>());
}

StrategySizeAlgorithm* CreateDefaultSizeAlgorithm() {
  return bindings::MakeGarbageCollected<DefaultSizeAlgorithm>();
}

void ScriptValueToObject(v8::Isolate* isolate,
                         v8::Local<v8::Value> value,
                         v8::Local<v8::Object>* object,
                         bindings::ExceptionState& exception_state) {
  CHECK(!value.IsEmpty());
  // Every object argument of the standard defaults to an empty object.
  if (value->IsUndefined()) {
    *object = v8::Object::New(isolate);
    return;
  }
  const bindings::TryRethrowScope rethrow_scope(isolate, exception_state);
  std::ignore = value->ToObject(isolate->GetCurrentContext()).ToLocal(object);
}

StrategyUnpacker::StrategyUnpacker(v8::Isolate* isolate,
                                   v8::Local<v8::Value> strategy,
                                   bindings::ExceptionState& exception_state) {
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Object> strategy_object;
  ScriptValueToObject(isolate, strategy, &strategy_object, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  // As in the constructor of WritableStream: step 2. Let size be ?
  // GetV(strategy, "size").
  const bindings::TryRethrowScope rethrow_scope(isolate, exception_state);
  if (!strategy_object->Get(context, NameString(isolate, "size"))
           .ToLocal(&size_)) {
    return;
  }
  // Step 3. Let highWaterMark be ? GetV(strategy, "highWaterMark").
  std::ignore =
      strategy_object->Get(context, NameString(isolate, "highWaterMark"))
          .ToLocal(&high_water_mark_);
}

StrategySizeAlgorithm* StrategyUnpacker::MakeSizeAlgorithm(
    v8::Isolate* isolate,
    bindings::ExceptionState& exception_state) const {
  CHECK(!size_.IsEmpty());
  // Step 6. Let sizeAlgorithm be ? MakeSizeAlgorithmFromSizeFunction(size).
  return MakeSizeAlgorithmFromSizeFunction(isolate, size_, exception_state);
}

double StrategyUnpacker::GetHighWaterMark(
    v8::Isolate* isolate,
    int default_value,
    bindings::ExceptionState& exception_state) const {
  CHECK(!high_water_mark_.IsEmpty());
  // Step 7. If highWaterMark is undefined, let highWaterMark be 1.
  if (high_water_mark_->IsUndefined()) {
    return default_value;
  }
  v8::Local<v8::Number> high_water_mark_as_number;
  {
    const bindings::TryRethrowScope rethrow_scope(isolate, exception_state);
    if (!high_water_mark_->ToNumber(isolate->GetCurrentContext())
             .ToLocal(&high_water_mark_as_number)) {
      return 0.0;
    }
  }
  // Step 8. Set highWaterMark to ? ValidateAndNormalizeHighWaterMark(
  // highWaterMark).
  return ValidateAndNormalizeHighWaterMark(high_water_mark_as_number->Value(),
                                           exception_state);
}

bool StrategyUnpacker::IsSizeUndefined() const {
  return size_->IsUndefined();
}

v8::Local<v8::Value> CreateTypeError(v8::Isolate* isolate,
                                     std::string_view message) {
  return v8::Exception::TypeError(
      bindings::ToV8StringFromUtf8(isolate, message));
}

v8::Local<v8::Value> CreateDOMException(v8::Isolate* isolate,
                                        std::string_view name,
                                        std::string_view message) {
  const bindings::PerIsolateData::DOMExceptionHooks& hooks =
      bindings::PerIsolateData::From(isolate)->dom_exception_hooks();
  CHECK(hooks.create);
  return hooks.create(isolate, name, message);
}

void EnqueueMicrotask(v8::Isolate* isolate, base::OnceClosure task) {
  isolate->EnqueueMicrotask(
      &RunQueuedMicrotask,
      new QueuedMicrotask{std::move(task),
                          v8::Global<v8::Context>(
                              isolate, isolate->GetCurrentContext())});
}

}  // namespace mitto
