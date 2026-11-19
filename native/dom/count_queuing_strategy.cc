#include "mitto/dom/count_queuing_strategy.h"

#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/script_function.h"
#include "mitto/bindings/v8_count_queuing_strategy.h"
#include "mitto/dom/queuing_strategy_common.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

namespace {

class CountQueuingStrategySizeFunction final : public bindings::ScriptFunction {
 public:
  static v8::Local<v8::Function> CreateFunction(v8::Isolate* isolate) {
    auto* const self =
        bindings::MakeGarbageCollected<CountQueuingStrategySizeFunction>();
    // Steps 2 and 4. Let F be ! CreateBuiltinFunction(steps, « », the realm),
    // and perform ! SetFunctionLength(F, 0).
    const v8::Local<v8::Function> function = self->ToV8Function(isolate);
    // Step 3. Perform ! SetFunctionName(F, "size").
    function->SetName(v8::String::NewFromUtf8Literal(isolate, "size"));
    return function;
  }

  void CallRaw(const v8::FunctionCallbackInfo<v8::Value>& info) override {
    // Step 1.1. Return 1.
    info.GetReturnValue().Set(v8::Integer::New(info.GetIsolate(), 1));
  }
};

}  // namespace

// static
CountQueuingStrategy* CountQueuingStrategy::Create(
    const bindings::QueuingStrategyInit& init) {
  return bindings::MakeGarbageCollected<CountQueuingStrategy>(
      *init.high_water_mark);
}

CountQueuingStrategy::CountQueuingStrategy(double high_water_mark)
    : high_water_mark_(high_water_mark) {}

CountQueuingStrategy::~CountQueuingStrategy() = default;

const bindings::WrapperTypeInfo* CountQueuingStrategy::GetWrapperTypeInfo()
    const {
  return &bindings::V8CountQueuingStrategy::kWrapperTypeInfo;
}

v8::Local<v8::Value> CountQueuingStrategy::Size() const {
  // Step 5. The global object's count queuing strategy size function.
  return GetCachedSizeFunction(v8::Isolate::GetCurrent(),
                               "mitto:CountQueuingStrategy size",
                               &CountQueuingStrategySizeFunction::CreateFunction);
}

}  // namespace mitto
