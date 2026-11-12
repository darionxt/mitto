// Conversions between V8's values and the C++ values that the Runtime's
// classes take and return, and the checks of a binding call's receiver.
//
// Upstream: third_party/blink/renderer/bindings/core/v8/native_value_traits_impl.h
// Upstream: third_party/blink/renderer/bindings/core/v8/to_v8_traits.h
// Upstream: third_party/blink/renderer/bindings/core/v8/generated_code_helper.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/script_iterator.cc

#ifndef MITTO_BINDINGS_V8_CONVERSIONS_H_
#define MITTO_BINDINGS_V8_CONVERSIONS_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/function_ref.h"
#include "base/memory/raw_ref.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/wrapper_type_info.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function-callback.h"
#include "v8/include/v8-function.h"
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

// Converts `value` to a double or float, by ToNumber(). For one that is not
// unrestricted, `restricted`, throws a TypeError naming `type_name` for a value
// that is not finite. Returns false if anything threw.
bool FromV8Double(v8::Isolate* isolate,
                  v8::Local<v8::Value> value,
                  bool restricted,
                  std::string_view type_name,
                  double* result);

// Converts `value` to a ByteString: a DOMString whose code units are all at
// most 0xFF, each of which becomes a byte. Throws a TypeError for one that is
// not, and returns false if anything threw.
bool FromV8ByteString(v8::Isolate* isolate,
                      v8::Local<v8::Value> value,
                      std::string* result);

// Converts `value` to one of `values`, the strings of the enumeration named
// `type_name`, as a DOMString. Throws a TypeError for another string, and
// returns false if anything threw.
bool FromV8Enumeration(v8::Isolate* isolate,
                       v8::Local<v8::Value> value,
                       base::span<const std::u16string_view> values,
                       std::string_view type_name,
                       std::u16string* result);

// Web IDL's conversion of `value` to a sequence: gets its @@iterator and calls
// `each` with each item, until `each` returns false. Throws a TypeError if
// `value` is not an object or has no @@iterator, and returns false if
// anything threw.
bool IterateSequence(v8::Isolate* isolate,
                     v8::Local<v8::Value> value,
                     ExceptionState& exception_state,
                     base::FunctionRef<bool(v8::Local<v8::Value>)> each);

// The same, with the @@iterator method that a union's conversion got.
bool IterateSequenceWithMethod(
    v8::Isolate* isolate,
    v8::Local<v8::Object> object,
    v8::Local<v8::Function> method,
    base::FunctionRef<bool(v8::Local<v8::Value>)> each);

// Gets the @@iterator of `object` into `method`, which stays empty if it is
// undefined or null. Returns false if that threw, or threw a TypeError because
// it is not callable.
bool GetIteratorMethod(v8::Isolate* isolate,
                       v8::Local<v8::Object> object,
                       v8::Local<v8::Function>* method);

// Web IDL's conversion of `value` to a record: calls `each` with the key and
// the value of each own enumerable property, in the order of
// [[OwnPropertyKeys]], until `each` returns false. Throws a TypeError naming
// `type_name` if `value` is not an object, and returns false if anything threw.
bool IterateRecord(
    v8::Isolate* isolate,
    v8::Local<v8::Value> value,
    ExceptionState& exception_state,
    std::string_view type_name,
    base::FunctionRef<bool(v8::Local<v8::Value>, v8::Local<v8::Value>)> each);

v8::Local<v8::String> ToV8String(v8::Isolate* isolate,
                                 std::u16string_view text);

// A ByteString, whose bytes are each a code unit.
v8::Local<v8::String> ToV8ByteString(v8::Isolate* isolate,
                                     std::string_view bytes);
v8::Local<v8::Value> ToV8NullableByteString(
    v8::Isolate* isolate,
    const std::optional<std::string>& bytes);
v8::Local<v8::Value> ToV8ByteStringArray(v8::Isolate* isolate,
                                         const std::vector<std::string>& texts);
// The string, or null.
v8::Local<v8::Value> ToV8NullableString(
    v8::Isolate* isolate,
    const std::optional<std::u16string>& text);
// An array of the strings.
v8::Local<v8::Value> ToV8StringArray(v8::Isolate* isolate,
                                     const std::vector<std::u16string>& texts);
v8::Local<v8::String> ToV8StringFromUtf8(v8::Isolate* isolate,
                                         std::string_view text);

// A long long or an unsigned long long, as Blink's ToV8Traits give them: an
// integer where it fits 32 bits, and otherwise a number, since v8::Integer
// holds no more.
// Upstream: third_party/blink/renderer/bindings/core/v8/to_v8_traits.h
v8::Local<v8::Value> ToV8Int64(v8::Isolate* isolate, int64_t value);
v8::Local<v8::Value> ToV8Uint64(v8::Isolate* isolate, uint64_t value);

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
// Chrome reports it to the console: through the isolate's exception reporter,
// or on standard error if it has none.
void ReportException(v8::Isolate* isolate, v8::Local<v8::Value> exception);

// What a binding call that returns a promise does with an error thrown before
// it returns: the call gives a promise rejected with it, as Web IDL says,
// instead of throwing.
// Upstream: third_party/blink/renderer/bindings/core/v8/generated_code_helper.h
class ExceptionToRejectPromiseScope {
 public:
  explicit ExceptionToRejectPromiseScope(
      const v8::FunctionCallbackInfo<v8::Value>& info);
  ExceptionToRejectPromiseScope(const ExceptionToRejectPromiseScope&) = delete;
  ExceptionToRejectPromiseScope& operator=(
      const ExceptionToRejectPromiseScope&) = delete;
  ~ExceptionToRejectPromiseScope();

 private:
  const raw_ref<const v8::FunctionCallbackInfo<v8::Value>> info_;
  v8::TryCatch try_catch_;
};

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_V8_CONVERSIONS_H_
