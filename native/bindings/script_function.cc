#include "mitto/bindings/script_function.h"

#include "base/notreached.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/wrapper_type_info.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-template.h"

namespace mitto::bindings {

namespace {

// What the JavaScript function holds as its data: an object that wraps the
// ScriptFunction, as Blink's FunctionHolder is, so that V8's collector keeps
// the ScriptFunction while the function lives.
class FunctionHolder final : public v8::Object::Wrappable {
 public:
  static v8::Local<v8::Function> Create(v8::Isolate* isolate,
                                        ScriptFunction* function) {
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();
    const v8::Local<v8::Object> data = PerIsolateData::From(isolate)
                                           ->HolderTemplate(isolate)
                                           ->NewInstance(context)
                                           .ToLocalChecked();
    v8::Object::Wrap<kFunctionHolderTag>(
        isolate, data, MakeGarbageCollected<FunctionHolder>(function));
    return v8::Function::New(context, &CallCallback, data, function->Length(),
                             v8::ConstructorBehavior::kThrow)
        .ToLocalChecked();
  }

  explicit FunctionHolder(ScriptFunction* function) : function_(function) {}

  const char* GetHumanReadableName() const override {
    return "ScriptFunction";
  }

  void Trace(cppgc::Visitor* visitor) const override {
    v8::Object::Wrappable::Trace(visitor);
    visitor->Trace(function_);
  }

 private:
  static void CallCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
    FunctionHolder* const holder =
        v8::Object::Unwrap<kFunctionHolderTag, FunctionHolder>(
            info.GetIsolate(), info.Data().As<v8::Object>());
    holder->function_->CallRaw(info);
  }

  const cppgc::Member<ScriptFunction> function_;
};

}  // namespace

ScriptFunction::ScriptFunction() = default;

ScriptFunction::~ScriptFunction() = default;

v8::Local<v8::Value> ScriptFunction::Call(v8::Isolate* isolate,
                                          v8::Local<v8::Value> value) {
  NOTREACHED();
}

void ScriptFunction::CallRaw(const v8::FunctionCallbackInfo<v8::Value>& info) {
  const v8::Local<v8::Value> result = Call(info.GetIsolate(), info[0]);
  if (!result.IsEmpty()) {
    info.GetReturnValue().Set(result);
  }
}

int ScriptFunction::Length() const {
  return 0;
}

void ScriptFunction::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(function_);
}

v8::Local<v8::Function> ScriptFunction::ToV8Function(v8::Isolate* isolate) {
  if (function_.IsEmpty()) {
    function_.Reset(isolate, FunctionHolder::Create(isolate, this));
  }
  return function_.Get(isolate);
}

}  // namespace mitto::bindings
