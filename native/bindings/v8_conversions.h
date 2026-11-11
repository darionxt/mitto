// Conversions between V8's values and the C++ values that the Runtime's
// classes take and return, and the checks of a binding call's receiver.
//
// Upstream: third_party/blink/renderer/bindings/core/v8/native_value_traits_impl.h
// Upstream: third_party/blink/renderer/bindings/core/v8/to_v8_traits.h

#ifndef MITTO_BINDINGS_V8_CONVERSIONS_H_
#define MITTO_BINDINGS_V8_CONVERSIONS_H_

#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/wrapper_type_info.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-function-callback.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-value.h"

namespace mitto::bindings {

// Converts `value` to a DOMString, as String() would, and returns false if that
// threw.
bool FromV8String(v8::Isolate* isolate,
                  v8::Local<v8::Value> value,
                  std::u16string* result);

v8::Local<v8::String> ToV8String(v8::Isolate* isolate,
                                 std::u16string_view text);
v8::Local<v8::String> ToV8StringFromUtf8(v8::Isolate* isolate,
                                         std::string_view text);

// The wrapper of `object`, or null.
v8::Local<v8::Value> ToV8(v8::Isolate* isolate, ScriptWrappable* object);

// An array of the objects' wrappers.
template <typename T>
v8::Local<v8::Value> ToV8Array(v8::Isolate* isolate,
                               const std::vector<T*>& objects) {
  std::vector<v8::Local<v8::Value>> wrappers;
  wrappers.reserve(objects.size());
  for (T* const object : objects) {
    wrappers.push_back(ToV8(isolate, object));
  }
  return v8::Array::New(isolate, wrappers.data(), wrappers.size());
}

// The C++ object that `value` wraps, if it is an object of the interface, or
// of one that inherits from it, and otherwise null.
template <typename T>
T* ToWrappableOf(v8::Isolate* isolate,
                 const WrapperTypeInfo& info,
                 v8::Local<v8::Value> value) {
  if (!PerIsolateData::From(isolate)->HasInstance(isolate, info, value)) {
    return nullptr;
  }
  return v8::Object::Unwrap<T>(
      isolate, value.As<v8::Object>(),
      v8::CppHeapPointerTagRange(info.tag, info.max_subclass_tag));
}

// The C++ object of a call's receiver. V8 checked the receiver against the
// member's signature already, and threw "Illegal invocation" if it did not
// pass. This throws the same for a receiver that is not wrapped yet, such as
// during its own constructor.
template <typename T>
T* ReceiverOf(const v8::FunctionCallbackInfo<v8::Value>& info,
              const WrapperTypeInfo& type) {
  T* const object = ToWrappableOf<T>(info.GetIsolate(), type, info.This());
  if (!object) {
    ThrowTypeError(info.GetIsolate(), "Illegal invocation");
  }
  return object;
}

// Reports an exception that a page's callback threw and nothing caught, as
// Chrome reports it to the console: on standard error.
void ReportException(v8::Isolate* isolate, v8::Local<v8::Value> exception);

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_V8_CONVERSIONS_H_
