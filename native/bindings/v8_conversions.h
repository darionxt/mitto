// Conversions between V8's values and the C++ values that the Runtime's
// classes take and return, and the checks of a binding call's receiver.
//
// Upstream: third_party/blink/renderer/bindings/core/v8/native_value_traits_impl.h
// Upstream: third_party/blink/renderer/bindings/core/v8/to_v8_traits.h

#ifndef MITTO_BINDINGS_V8_CONVERSIONS_H_
#define MITTO_BINDINGS_V8_CONVERSIONS_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
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

// The same for a nullable DOMString, which null and undefined convert to
// nullopt.
bool FromV8NullableString(v8::Isolate* isolate,
                          v8::Local<v8::Value> value,
                          std::optional<std::u16string>* result);

// The same for [LegacyNullToEmptyString] DOMString, which null converts to the
// empty string.
bool FromV8StringNullToEmpty(v8::Isolate* isolate,
                             v8::Local<v8::Value> value,
                             std::u16string* result);

// Converts `value` to a USVString: a DOMString whose lone surrogates become
// U+FFFD.
bool FromV8USVString(v8::Isolate* isolate,
                     v8::Local<v8::Value> value,
                     std::u16string* result);
bool FromV8NullableUSVString(v8::Isolate* isolate,
                             v8::Local<v8::Value> value,
                             std::optional<std::u16string>* result);

// Converts `value` to one of Web IDL's integer types of 32 bits or fewer, as
// ConvertToInt() does without [EnforceRange] or [Clamp]: ToNumber(), then the
// integer modulo 2 to the type's bits, which is what ToInt32() or ToUint32()
// and then the type's low bits give. Returns false if that threw.
template <typename T>
bool FromV8Integer(v8::Isolate* isolate, v8::Local<v8::Value> value, T* result)
  requires(std::is_integral_v<T> && sizeof(T) <= 4)
{
  if constexpr (std::is_signed_v<T>) {
    int32_t number;
    if (!value->Int32Value(isolate->GetCurrentContext()).To(&number)) {
      return false;
    }
    *result = static_cast<T>(number);
  } else {
    uint32_t number;
    if (!value->Uint32Value(isolate->GetCurrentContext()).To(&number)) {
      return false;
    }
    *result = static_cast<T>(number);
  }
  return true;
}

v8::Local<v8::String> ToV8String(v8::Isolate* isolate,
                                 std::u16string_view text);
// The string, or null.
v8::Local<v8::Value> ToV8NullableString(
    v8::Isolate* isolate,
    const std::optional<std::u16string>& text);
// An array of the strings.
v8::Local<v8::Value> ToV8StringArray(v8::Isolate* isolate,
                                     const std::vector<std::u16string>& texts);
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
