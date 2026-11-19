// The Streams Standard's miscellaneous operations, which make a stream's
// algorithms from what a page gives a constructor, as Blink's do.
//
// Upstream: third_party/blink/renderer/core/streams/miscellaneous_operations.h
// Upstream: third_party/blink/renderer/core/streams/miscellaneous_operations.cc

#ifndef MITTO_DOM_MISCELLANEOUS_OPERATIONS_H_
#define MITTO_DOM_MISCELLANEOUS_OPERATIONS_H_

#include <stddef.h>

#include <string_view>

#include "base/functional/callback.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-maybe.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-promise.h"

namespace v8 {
class Function;
class Isolate;
class Value;
}  // namespace v8

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class StrategySizeAlgorithm;
class StreamAlgorithm;
class StreamStartAlgorithm;

// CreateAlgorithmFromUnderlyingMethod(), which takes the arguments when it
// runs rather than how many, and at most one extra argument: empty for none.
// `method_name_for_error` names the method in errors, such as
// "underlyingSink.write". Returns null if reading the method threw.
StreamAlgorithm* CreateAlgorithmFromUnderlyingMethod(
    v8::Isolate* isolate,
    v8::Local<v8::Object> underlying_object,
    const char* method_name,
    const char* method_name_for_error,
    v8::MaybeLocal<v8::Value> extra_arg,
    bindings::ExceptionState& exception_state);

// Reads `method_name` of `object`, and throws a TypeError if it is neither a
// function nor undefined, which names it as `name_for_error`.
v8::MaybeLocal<v8::Value> ResolveMethod(
    v8::Isolate* isolate,
    v8::Local<v8::Object> object,
    const char* method_name,
    const char* name_for_error,
    bindings::ExceptionState& exception_state);

// As CreateAlgorithmFromUnderlyingMethod(), for a method that has been read
// already and is a function.
StreamAlgorithm* CreateAlgorithmFromResolvedMethod(
    v8::Isolate* isolate,
    v8::Local<v8::Object> underlying_object,
    v8::Local<v8::Value> method,
    v8::MaybeLocal<v8::Value> extra_arg);

// A start algorithm that reads and calls `underlying_object`'s start() when it
// runs. `method_name_for_error` must outlive it.
StreamStartAlgorithm* CreateStartAlgorithm(
    v8::Isolate* isolate,
    v8::Local<v8::Object> underlying_object,
    const char* method_name_for_error,
    v8::Local<v8::Value> controller);

// A start algorithm of a byte stream, whose start() has been read already.
StreamStartAlgorithm* CreateByteStreamStartAlgorithm(
    v8::Isolate* isolate,
    v8::Local<v8::Object> underlying_object,
    v8::Local<v8::Value> method,
    v8::Local<v8::Value> controller);

// Algorithms that return a promise resolved with undefined.
StreamStartAlgorithm* CreateTrivialStartAlgorithm();
StreamAlgorithm* CreateTrivialStreamAlgorithm();

// A strategy with no size() and `high_water_mark`, with a null prototype,
// which behaves as a CountQueuingStrategy does.
v8::Local<v8::Object> CreateTrivialQueuingStrategy(v8::Isolate* isolate,
                                                   size_t high_water_mark);

// InvokeOrNoop() with one argument.
v8::MaybeLocal<v8::Value> CallOrNoop1(
    v8::Isolate* isolate,
    v8::Local<v8::Object> object,
    const char* method_name,
    const char* name_for_error,
    v8::Local<v8::Value> arg0,
    bindings::ExceptionState& exception_state);

// PromiseCall(F, V, args): a promise of what calling `method` on `recv` gives,
// rejected with what it throws.
v8::Local<v8::Promise> PromiseCall(v8::Isolate* isolate,
                                   v8::Local<v8::Function> method,
                                   v8::Local<v8::Object> recv,
                                   int argc,
                                   v8::Local<v8::Value> argv[]);

// ValidateAndNormalizeHighWaterMark(), for a value that is a number already.
double ValidateAndNormalizeHighWaterMark(
    double high_water_mark,
    bindings::ExceptionState& exception_state);

// MakeSizeAlgorithmFromSizeFunction().
StrategySizeAlgorithm* MakeSizeAlgorithmFromSizeFunction(
    v8::Isolate* isolate,
    v8::Local<v8::Value> size,
    bindings::ExceptionState& exception_state);

// A size algorithm that gives 1 for every chunk.
StrategySizeAlgorithm* CreateDefaultSizeAlgorithm();

// `value` as an object: a new empty one for undefined, as each object argument
// of the standard defaults to one, and a TypeError for null.
void ScriptValueToObject(v8::Isolate* isolate,
                         v8::Local<v8::Value> value,
                         v8::Local<v8::Object>* object,
                         bindings::ExceptionState& exception_state);

// Unpacks a strategy for a constructor of ReadableStream, WritableStream or
// TransformStream: reads its `size` and `highWaterMark`, which may run a
// page's code.
class StrategyUnpacker final {
 public:
  StrategyUnpacker(v8::Isolate* isolate,
                   v8::Local<v8::Value> strategy,
                   bindings::ExceptionState& exception_state);
  StrategyUnpacker(const StrategyUnpacker&) = delete;
  StrategyUnpacker& operator=(const StrategyUnpacker&) = delete;
  ~StrategyUnpacker() = default;

  // MakeSizeAlgorithmFromSizeFunction() of its size, which may throw.
  StrategySizeAlgorithm* MakeSizeAlgorithm(
      v8::Isolate* isolate,
      bindings::ExceptionState& exception_state) const;

  // Its highWaterMark, validated, or `default_value` if it is undefined.
  double GetHighWaterMark(v8::Isolate* isolate,
                          int default_value,
                          bindings::ExceptionState& exception_state) const;

  bool IsSizeUndefined() const;

 private:
  v8::Local<v8::Value> size_;
  v8::Local<v8::Value> high_water_mark_;
};

// A TypeError and a DOMException with `message`, as V8ThrowException and
// V8ThrowDOMException make them for the streams, without throwing them.
v8::Local<v8::Value> CreateTypeError(v8::Isolate* isolate,
                                     std::string_view message);
v8::Local<v8::Value> CreateDOMException(v8::Isolate* isolate,
                                        std::string_view name,
                                        std::string_view message);

// Runs `task` from the microtask queue in the current context, as Blink's
// ScriptState::EnqueueMicrotask() does. The task holds what it needs, such as a
// cppgc::Persistent of its object and a v8::Global of a value.
void EnqueueMicrotask(v8::Isolate* isolate, base::OnceClosure task);

}  // namespace mitto

#endif  // MITTO_DOM_MISCELLANEOUS_OPERATIONS_H_
