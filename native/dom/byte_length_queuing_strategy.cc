#include "mitto/dom/byte_length_queuing_strategy.h"

#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/script_function.h"
#include "mitto/bindings/v8_byte_length_queuing_strategy.h"
#include "mitto/dom/queuing_strategy_common.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

namespace {

class ByteLengthQueuingStrategySizeFunction final
    : public bindings::ScriptFunction {
 public:
  static v8::Local<v8::Function> CreateFunction(v8::Isolate* isolate) {
    auto* const self =
        bindings::MakeGarbageCollected<ByteLengthQueuingStrategySizeFunction>();
    // Steps 2 and 4. Let F be ! CreateBuiltinFunction(steps, « », the realm),
    // and perform ! SetFunctionLength(F, 1).
    const v8::Local<v8::Function> function = self->ToV8Function(isolate);
    // Step 3. Perform ! SetFunctionName(F, "size").
    function->SetName(v8::String::NewFromUtf8Literal(isolate, "size"));
    return function;
  }

  void CallRaw(const v8::FunctionCallbackInfo<v8::Value>& info) override {
    v8::Isolate* const isolate = info.GetIsolate();
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();
    const v8::Local<v8::Value> chunk = info[0];
    // Step 1.1. Return ? GetV(chunk, "byteLength"), which converts chunk to an
    // object first.
    v8::Local<v8::Object> chunk_as_object;
    if (!chunk->ToObject(context).ToLocal(&chunk_as_object)) {
      return;
    }
    v8::Local<v8::Value> byte_length;
    if (!chunk_as_object
             ->Get(context,
                   v8::String::NewFromUtf8Literal(
                       isolate, "byteLength", v8::NewStringType::kInternalized))
             .ToLocal(&byte_length)) {
      return;
    }
    info.GetReturnValue().Set(byte_length);
  }

  int Length() const override { return 1; }
};

v8::Local<v8::Function> CreateSizeFunction(v8::Isolate* isolate) {
  return ByteLengthQueuingStrategySizeFunction::CreateFunction(isolate);
}

}  // namespace

// static
ByteLengthQueuingStrategy* ByteLengthQueuingStrategy::Create(
    const bindings::QueuingStrategyInit& init) {
  return bindings::MakeGarbageCollected<ByteLengthQueuingStrategy>(
      *init.high_water_mark);
}

ByteLengthQueuingStrategy::ByteLengthQueuingStrategy(double high_water_mark)
    : high_water_mark_(high_water_mark) {}

ByteLengthQueuingStrategy::~ByteLengthQueuingStrategy() = default;

const bindings::WrapperTypeInfo* ByteLengthQueuingStrategy::GetWrapperTypeInfo()
    const {
  return &bindings::V8ByteLengthQueuingStrategy::kWrapperTypeInfo;
}

v8::Local<v8::Value> ByteLengthQueuingStrategy::Size() const {
  // Step 5. The global object's byte length queuing strategy size function.
  return GetCachedSizeFunction(v8::Isolate::GetCurrent(),
                               "mitto:ByteLengthQueuingStrategy size",
                               &CreateSizeFunction);
}

}  // namespace mitto
