// Conversions between V8's values and the C++ values that the Runtime's
// classes take and return, and the checks of a binding call's receiver.
//
// Upstream: third_party/blink/renderer/bindings/core/v8/native_value_traits_impl.h
// Upstream: third_party/blink/renderer/bindings/core/v8/to_v8_traits.h
// Upstream: third_party/blink/renderer/bindings/core/v8/generated_code_helper.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/script_iterator.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/native_value_traits_buffer_sources.cc

#ifndef MITTO_BINDINGS_V8_CONVERSIONS_H_
#define MITTO_BINDINGS_V8_CONVERSIONS_H_

#include <stdint.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
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
#include "v8/include/v8-array-buffer.h"
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

// The same for long long and unsigned long long, as Blink's ToInt64() and
// ToUInt64() do: 0 for a value that is not finite, and otherwise its integer
// part modulo 2 to the 64th, as WTF's doubleToInteger() takes it.
template <typename T>
bool FromV8Integer(v8::Isolate* isolate, v8::Local<v8::Value> value, T* result)
  requires(std::is_integral_v<T> && sizeof(T) == 8)
{
  double number;
  if (!value->NumberValue(isolate->GetCurrentContext()).To(&number)) {
    return false;
  }
  uint64_t bits = 0;
  if (std::isfinite(number)) {
    // -2^64 < remainder < 2^64.
    const double remainder = std::fmod(
        std::trunc(number),
        static_cast<double>(std::numeric_limits<uint64_t>::max()) + 1.0);
    bits = remainder >= 0 ? static_cast<uint64_t>(remainder)
                          : std::numeric_limits<uint64_t>::max() -
                                static_cast<uint64_t>(-remainder) + 1;
  }
  *result = static_cast<T>(bits);
  return true;
}

// Converts `value` to one of Web IDL's integer types with [EnforceRange], as
// ConvertToInt() does: ToNumber(), then a TypeError naming `type_name` for a
// value that is not finite or whose integer part is outside the type's range,
// which for the 64-bit types is that of the integers a double holds exactly.
// Returns false if anything threw.
bool FromV8EnforceRange(v8::Isolate* isolate,
                        v8::Local<v8::Value> value,
                        std::string_view type_name,
                        double minimum,
                        double maximum,
                        ExceptionState& exception_state,
                        double* result);
template <typename T>
bool FromV8IntegerEnforceRange(v8::Isolate* isolate,
                               v8::Local<v8::Value> value,
                               std::string_view type_name,
                               ExceptionState& exception_state,
                               T* result)
  requires(std::is_integral_v<T>)
{
  // Blink's kJSMaxInteger, 2^53 - 1.
  constexpr double kMaxSafeInteger = 9007199254740991.0;
  constexpr double kMinimum =
      sizeof(T) == 8 ? (std::is_signed_v<T> ? -kMaxSafeInteger : 0.0)
                     : static_cast<double>(std::numeric_limits<T>::min());
  constexpr double kMaximum =
      sizeof(T) == 8 ? kMaxSafeInteger
                     : static_cast<double>(std::numeric_limits<T>::max());
  double number;
  if (!FromV8EnforceRange(isolate, value, type_name, kMinimum, kMaximum,
                          exception_state, &number)) {
    return false;
  }
  *result = static_cast<T>(number);
  return true;
}

// The same with [Clamp], as Blink's ToSmallerInt() and kin do it: ToNumber(),
// 0 for NaN, and otherwise the number within the type's range, truncated, as
// Blink's ClampTo() gives it.
template <typename T>
bool FromV8ClampedInteger(v8::Isolate* isolate,
                          v8::Local<v8::Value> value,
                          T* result)
  requires(std::is_integral_v<T> && sizeof(T) <= 4)
{
  double number;
  if (!value->NumberValue(isolate->GetCurrentContext()).To(&number)) {
    return false;
  }
  if (std::isnan(number)) {
    *result = 0;
    return true;
  }
  number =
      std::clamp(number, static_cast<double>(std::numeric_limits<T>::min()),
                 static_cast<double>(std::numeric_limits<T>::max()));
  *result = static_cast<T>(std::trunc(number));
  return true;
}

// Checks a view that converted to `type_name`, such as "Uint8Array", as
// NativeValueTraits<NotShared<T>> does: its buffer must be neither shared nor
// resizable. Throws a TypeError and returns false if it is.
bool CheckArrayBufferViewNotShared(v8::Local<v8::ArrayBufferView> view,
                                   std::string_view type_name,
                                   ExceptionState& exception_state);

// Converts `value`, an ArrayBuffer or a SharedArrayBuffer, to the ArrayBuffer
// member of a union, as NativeValueTraits<DOMArrayBuffer> does: a buffer that
// is not shared, not resizable and no larger than Blink takes. Throws and
// returns false for any other.
bool FromV8ArrayBufferMember(v8::Local<v8::Value> value,
                             ExceptionState& exception_state,
                             v8::Local<v8::ArrayBuffer>* result);

// Converts `value`, a view, to the ArrayBufferView member of a union, as
// NativeValueTraits<NotShared<DOMArrayBufferView>> does: one whose buffer is
// neither shared nor resizable, no larger than Blink takes. Throws and returns
// false for any other.
bool FromV8ArrayBufferViewMember(v8::Local<v8::Value> value,
                                 ExceptionState& exception_state,
                                 v8::Local<v8::ArrayBufferView>* result);

// The bytes of a [PassAsSpan] argument, as Blink's ByteSpanWithInlineStorage
// holds them with its detach check: those of a buffer, which a page's script
// may detach while the call's later arguments convert, so that the call then
// reads none, or a copy of those of a view that V8 keeps on its heap, which
// no script can take away.
class ByteSpanArgument {
 public:
  // V8 keeps a typed array of at most this many bytes on its heap, and Blink
  // copies one into storage of this size.
  static constexpr size_t kInlineStorageSize = 64;

  ByteSpanArgument();
  ByteSpanArgument(const ByteSpanArgument&);
  ByteSpanArgument& operator=(const ByteSpanArgument&);
  ~ByteSpanArgument();

  // `length` bytes of `buffer` from `offset`.
  void SetBuffer(v8::Local<v8::ArrayBuffer> buffer,
                 size_t offset,
                 size_t length);
  // A copy of the bytes of `view`, which has no buffer of its own yet.
  void SetCopy(v8::Local<v8::ArrayBufferView> view);

  size_t size() const { return length_; }
  // The bytes, none if the buffer has been detached since.
  base::span<const uint8_t> get() const;

 private:
  v8::Local<v8::ArrayBuffer> buffer_;
  size_t offset_ = 0;
  size_t length_ = 0;
  bool copied_ = false;
  std::array<uint8_t, kInlineStorageSize> inline_storage_{};
};

// What the class's method takes for a [PassAsSpan] argument, once every
// argument has converted.
base::span<const uint8_t> SpanAtCall(const ByteSpanArgument& argument);
std::optional<base::span<const uint8_t>> SpanAtCall(
    const std::optional<ByteSpanArgument>& argument);

// Converts the argument at `argument_index` of a [PassAsSpan] BufferSource to
// its bytes, as Blink's NativeValueTraits of PassAsSpan do: an ArrayBuffer or
// a view on one that is neither shared nor resizable, and no larger than
// Blink takes. Throws and returns false for anything else.
bool FromV8ByteSpan(v8::Local<v8::Value> value,
                    int argument_index,
                    ExceptionState& exception_state,
                    ByteSpanArgument* result);

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

// Whether `value` is an object whose @@iterator is neither undefined nor null,
// as Blink's IsEsIterableObject() decides it for Web IDL's overload
// resolution. Returns false with `*threw` set if reading it threw, or threw a
// TypeError because it is not callable.
bool IsIterableObject(v8::Isolate* isolate,
                      v8::Local<v8::Value> value,
                      bool* threw);

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
// The number, or null.
v8::Local<v8::Value> ToV8NullableNumber(v8::Isolate* isolate,
                                        std::optional<double> number);
// The object, or null for an empty one.
v8::Local<v8::Value> ToV8NullableObject(v8::Isolate* isolate,
                                        v8::Local<v8::Object> object);
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
