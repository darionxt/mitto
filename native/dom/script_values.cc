#include "mitto/dom/script_values.h"

#include <tuple>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/single_thread_task_runner.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_window.h"
#include "mitto/dom/window.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

// What a task settles a promise with, made in that task.
using MakeValue = base::OnceCallback<v8::Local<v8::Value>(v8::Isolate*)>;

void Settle(v8::Isolate* isolate,
            v8::Global<v8::Context> context_handle,
            v8::Global<v8::Promise::Resolver> resolver_handle,
            MakeValue make_value,
            bool reject) {
  const v8::HandleScope handle_scope(isolate);
  const v8::Local<v8::Context> context = context_handle.Get(isolate);
  const v8::Context::Scope context_scope(context);
  // A resolver of a context that has gone settles nothing, as Blink's does
  // once its execution context is destroyed.
  const Window* const window =
      bindings::V8Window::ToWrappable(isolate, context->Global());
  if (!window || !window->HasContext()) {
    return;
  }
  const v8::Local<v8::Promise::Resolver> resolver =
      resolver_handle.Get(isolate);
  const v8::Local<v8::Value> value = std::move(make_value).Run(isolate);
  if (reject) {
    std::ignore = resolver->Reject(context, value);
  } else {
    std::ignore = resolver->Resolve(context, value);
  }
}

void SettleFromTask(v8::Isolate* isolate,
                    v8::Local<v8::Promise::Resolver> resolver,
                    MakeValue make_value,
                    bool reject) {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          &Settle, base::Unretained(isolate),
          v8::Global<v8::Context>(isolate, isolate->GetCurrentContext()),
          v8::Global<v8::Promise::Resolver>(isolate, resolver),
          std::move(make_value), reject));
}

}  // namespace

v8::Local<v8::Array> FrozenArray(
    v8::Isolate* isolate,
    const std::vector<v8::Local<v8::Value>>& items) {
  std::vector<v8::Local<v8::Value>> copy = items;
  const v8::Local<v8::Array> array =
      v8::Array::New(isolate, copy.data(), copy.size());
  std::ignore = array->SetIntegrityLevel(isolate->GetCurrentContext(),
                                         v8::IntegrityLevel::kFrozen);
  return array;
}

v8::Local<v8::Array> FrozenArrayOfStrings(
    v8::Isolate* isolate,
    const std::vector<std::u16string>& items) {
  std::vector<v8::Local<v8::Value>> values;
  values.reserve(items.size());
  for (const std::u16string& item : items) {
    values.push_back(bindings::ToV8String(isolate, item));
  }
  return FrozenArray(isolate, values);
}

v8::Local<v8::Object> PlainObject(
    v8::Isolate* isolate,
    const std::vector<std::pair<std::string_view, v8::Local<v8::Value>>>&
        members) {
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::Local<v8::Object> object = v8::Object::New(isolate);
  for (const auto& [name, value] : members) {
    std::ignore = object->CreateDataProperty(
        context, bindings::ToV8StringFromUtf8(isolate, name), value);
  }
  return object;
}

void ResolveFromTask(v8::Isolate* isolate,
                     v8::Local<v8::Promise::Resolver> resolver,
                     v8::Local<v8::Value> value) {
  SettleFromTask(
      isolate, resolver,
      base::BindOnce([](v8::Global<v8::Value> held,
                        v8::Isolate* isolate) { return held.Get(isolate); },
                     v8::Global<v8::Value>(isolate, value)),
      /*reject=*/false);
}

void ResolveFromTaskWith(
    v8::Isolate* isolate,
    v8::Local<v8::Promise::Resolver> resolver,
    base::OnceCallback<v8::Local<v8::Value>(v8::Isolate*)> make_value) {
  SettleFromTask(isolate, resolver, std::move(make_value), /*reject=*/false);
}

void RejectWithDOMExceptionFromTask(v8::Isolate* isolate,
                                    v8::Local<v8::Promise::Resolver> resolver,
                                    std::string name,
                                    std::string message) {
  SettleFromTask(isolate, resolver,
                 base::BindOnce(
                     [](const std::string& name, const std::string& message,
                        v8::Isolate* isolate) {
                       return bindings::PerIsolateData::From(isolate)
                           ->dom_exception_hooks()
                           .create(isolate, name, message);
                     },
                     std::move(name), std::move(message)),
                 /*reject=*/true);
}

}  // namespace mitto
