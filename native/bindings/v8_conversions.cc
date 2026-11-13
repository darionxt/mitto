#include "mitto/bindings/v8_conversions.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include <cmath>
#include <utility>

#include "base/compiler_specific.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_messages.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-promise.h"

namespace mitto::bindings {

bool FromV8String(v8::Isolate* isolate,
                  v8::Local<v8::Value> value,
                  std::u16string* result) {
  v8::Local<v8::String> string;
  if (value->IsString()) {
    string = value.As<v8::String>();
  } else if (!value->ToString(isolate->GetCurrentContext()).ToLocal(&string)) {
    return false;
  }
  const uint32_t length = string->Length();
  result->resize(length);
  string->WriteV2(isolate, 0, length,
                  reinterpret_cast<uint16_t*>(result->data()));
  return true;
}

bool FromV8NullableString(v8::Isolate* isolate,
                          v8::Local<v8::Value> value,
                          std::optional<std::u16string>* result) {
  if (value->IsNullOrUndefined()) {
    result->reset();
    return true;
  }
  std::u16string string;
  if (!FromV8String(isolate, value, &string)) {
    return false;
  }
  *result = std::move(string);
  return true;
}

bool FromV8StringNullToEmpty(v8::Isolate* isolate,
                             v8::Local<v8::Value> value,
                             std::u16string* result) {
  if (value->IsNull()) {
    result->clear();
    return true;
  }
  return FromV8String(isolate, value, result);
}

bool FromV8USVString(v8::Isolate* isolate,
                     v8::Local<v8::Value> value,
                     std::u16string* result) {
  if (!FromV8String(isolate, value, result)) {
    return false;
  }
  // Web IDL's "convert a JavaScript value to a USVString": each lone surrogate
  // becomes U+FFFD.
  std::u16string& text = *result;
  for (size_t i = 0; i < text.size(); ++i) {
    const char16_t unit = text[i];
    if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < text.size() &&
        text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
      ++i;
    } else if (unit >= 0xD800 && unit <= 0xDFFF) {
      text[i] = 0xFFFD;
    }
  }
  return true;
}

bool FromV8NullableUSVString(v8::Isolate* isolate,
                             v8::Local<v8::Value> value,
                             std::optional<std::u16string>* result) {
  if (value->IsNullOrUndefined()) {
    result->reset();
    return true;
  }
  std::u16string string;
  if (!FromV8USVString(isolate, value, &string)) {
    return false;
  }
  *result = std::move(string);
  return true;
}

bool CheckArrayBufferViewNotShared(v8::Local<v8::ArrayBufferView> view,
                                   std::string_view type_name,
                                   ExceptionState& exception_state) {
  // A view that V8 keeps on its heap has no buffer of its own yet, and so is
  // neither shared nor resizable.
  if (!view->HasBuffer()) {
    return true;
  }
  const v8::Local<v8::ArrayBuffer> buffer = view->Buffer();
  if (buffer->GetBackingStore()->IsShared()) {
    exception_state.ThrowTypeError(base::StrCat(
        {"The provided ", type_name, " value must not be shared."}));
    return false;
  }
  if (buffer->IsResizableByUserJavaScript()) {
    exception_state.ThrowTypeError(base::StrCat(
        {"The provided ", type_name, " value must not be resizable."}));
    return false;
  }
  return true;
}

namespace {

// The bytes of `buffer` from `offset`, `length` of them.
base::span<const uint8_t> BytesOf(v8::Local<v8::ArrayBuffer> buffer,
                                  size_t offset,
                                  size_t length) {
  if (!length) {
    return {};
  }
  // SAFETY: V8 gives the buffer's data and its length in bytes, and a view's
  // bytes lie within its buffer.
  return UNSAFE_BUFFERS(base::span(static_cast<const uint8_t*>(buffer->Data()),
                                   buffer->ByteLength()))
      .subspan(offset, length);
}

}  // namespace

// Upstream: third_party/blink/renderer/bindings/core/v8/native_value_traits_impl.h
// Upstream: third_party/blink/renderer/bindings/core/v8/native_value_traits_impl.cc
bool FromV8ByteSpan(v8::Local<v8::Value> value,
                    int argument_index,
                    ExceptionState& exception_state,
                    base::span<const uint8_t>* result) {
  constexpr char kResizable[] =
      "The provided ArrayBuffer value must not be resizable";
  if (value->IsArrayBuffer()) {
    const v8::Local<v8::ArrayBuffer> buffer = value.As<v8::ArrayBuffer>();
    if (buffer->IsResizableByUserJavaScript()) {
      exception_state.ThrowTypeError(kResizable);
      return false;
    }
    *result = BytesOf(buffer, 0, buffer->ByteLength());
  } else if (value->IsArrayBufferView()) {
    const v8::Local<v8::ArrayBufferView> view = value.As<v8::ArrayBufferView>();
    // Blink copies the bytes of a view that V8 keeps on its heap, which moves;
    // asking for its buffer moves them off the heap instead, which a page
    // cannot see.
    const v8::Local<v8::ArrayBuffer> buffer = view->Buffer();
    if (buffer->IsResizableByUserJavaScript()) {
      exception_state.ThrowTypeError(kResizable);
      return false;
    }
    if (buffer->GetBackingStore()->IsShared()) {
      exception_state.ThrowTypeError(
          "The provided ArrayBufferView value must not be shared.");
      return false;
    }
    *result = BytesOf(buffer, view->ByteOffset(), view->ByteLength());
  } else {
    exception_state.ThrowTypeError(
        ExceptionMessages::ArgumentNotOfType(argument_index, "ArrayBuffer"));
    return false;
  }
  // partition_alloc::MaxAllocationSize(), the most that Blink takes: 2 GiB
  // less a super page of 2 MiB.
  constexpr size_t kMaxAllocationSize = (size_t{1} << 31) - (size_t{1} << 21);
  if (result->size() > kMaxAllocationSize) {
    exception_state.ThrowRangeError(
        "The ArrayBuffer/ArrayBufferView size exceeds the supported range.");
    *result = {};
    return false;
  }
  return true;
}

v8::Local<v8::Value> ToV8NullableString(
    v8::Isolate* isolate,
    const std::optional<std::u16string>& text) {
  if (!text) {
    return v8::Null(isolate);
  }
  return ToV8String(isolate, *text);
}

v8::Local<v8::Value> ToV8StringArray(v8::Isolate* isolate,
                                     const std::vector<std::u16string>& texts) {
  std::vector<v8::Local<v8::Value>> strings;
  strings.reserve(texts.size());
  for (const std::u16string& text : texts) {
    strings.push_back(ToV8String(isolate, text));
  }
  return v8::Array::New(isolate, strings.data(), strings.size());
}

v8::Local<v8::String> ToV8String(v8::Isolate* isolate,
                                 std::u16string_view text) {
  return v8::String::NewFromTwoByte(
             isolate, reinterpret_cast<const uint16_t*>(text.data()),
             v8::NewStringType::kNormal, base::checked_cast<int>(text.size()))
      .ToLocalChecked();
}

v8::Local<v8::String> ToV8StringFromUtf8(v8::Isolate* isolate,
                                         std::string_view text) {
  return v8::String::NewFromUtf8(isolate, text.data(),
                                 v8::NewStringType::kNormal,
                                 base::checked_cast<int>(text.size()))
      .ToLocalChecked();
}

v8::Local<v8::Value> ToV8(v8::Isolate* isolate, ScriptWrappable* object) {
  if (!object) {
    return v8::Null(isolate);
  }
  return object->ToV8(isolate);
}

bool FromV8Double(v8::Isolate* isolate,
                  v8::Local<v8::Value> value,
                  bool restricted,
                  std::string_view type_name,
                  double* result) {
  double number;
  if (!value->NumberValue(isolate->GetCurrentContext()).To(&number)) {
    return false;
  }
  if (restricted && !std::isfinite(number)) {
    ThrowTypeError(isolate, base::StrCat({"The provided ", type_name,
                                          " value is non-finite."}));
    return false;
  }
  *result = number;
  return true;
}

bool FromV8ByteString(v8::Isolate* isolate,
                      v8::Local<v8::Value> value,
                      std::string* result) {
  v8::Local<v8::String> string;
  if (value->IsString()) {
    string = value.As<v8::String>();
  } else if (!value->ToString(isolate->GetCurrentContext()).ToLocal(&string)) {
    return false;
  }
  if (!string->ContainsOnlyOneByte()) {
    ThrowTypeError(isolate, "String contains non ISO-8859-1 code point.");
    return false;
  }
  const uint32_t length = string->Length();
  result->resize(length);
  string->WriteOneByteV2(isolate, 0, length,
                         reinterpret_cast<uint8_t*>(result->data()));
  return true;
}

bool FromV8Enumeration(v8::Isolate* isolate,
                       v8::Local<v8::Value> value,
                       base::span<const std::u16string_view> values,
                       std::string_view type_name,
                       std::u16string* result) {
  std::u16string string;
  if (!FromV8String(isolate, value, &string)) {
    return false;
  }
  for (const std::u16string_view item : values) {
    if (item == string) {
      *result = std::move(string);
      return true;
    }
  }
  // As Blink's generated code helper says it.
  ThrowTypeError(
      isolate,
      base::StrCat({"The provided value '", base::UTF16ToUTF8(string),
                    "' is not a valid enum value of type ", type_name, "."}));
  return false;
}

bool GetIteratorMethod(v8::Isolate* isolate,
                       v8::Local<v8::Object> object,
                       v8::Local<v8::Function>* method) {
  v8::Local<v8::Value> value;
  if (!object
           ->Get(isolate->GetCurrentContext(), v8::Symbol::GetIterator(isolate))
           .ToLocal(&value)) {
    return false;
  }
  if (value->IsNullOrUndefined()) {
    *method = v8::Local<v8::Function>();
    return true;
  }
  if (!value->IsFunction()) {
    ThrowTypeError(isolate, "@@iterator must be a callable.");
    return false;
  }
  *method = value.As<v8::Function>();
  return true;
}

// Upstream: third_party/blink/renderer/bindings/core/v8/script_iterator.cc
bool IterateSequenceWithMethod(
    v8::Isolate* isolate,
    v8::Local<v8::Object> object,
    v8::Local<v8::Function> method,
    base::FunctionRef<bool(v8::Local<v8::Value>)> each) {
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Value> iterator;
  if (!method->Call(context, object, 0, nullptr).ToLocal(&iterator)) {
    return false;
  }
  if (!iterator->IsObject()) {
    ThrowTypeError(isolate, "Iterator object must be an object.");
    return false;
  }
  v8::Local<v8::Value> next;
  if (!iterator.As<v8::Object>()
           ->Get(context, v8::String::NewFromUtf8Literal(isolate, "next"))
           .ToLocal(&next)) {
    return false;
  }
  if (!next->IsFunction()) {
    ThrowTypeError(isolate, "Iterator object must have a callable next.");
    return false;
  }
  const v8::Local<v8::String> done_key =
      v8::String::NewFromUtf8Literal(isolate, "done");
  const v8::Local<v8::String> value_key =
      v8::String::NewFromUtf8Literal(isolate, "value");
  while (true) {
    v8::Local<v8::Value> result;
    if (!next.As<v8::Function>()
             ->Call(context, iterator, 0, nullptr)
             .ToLocal(&result)) {
      return false;
    }
    if (!result->IsObject()) {
      ThrowTypeError(isolate,
                     "The result of iterator.next() must be an object.");
      return false;
    }
    v8::Local<v8::Value> done;
    if (!result.As<v8::Object>()->Get(context, done_key).ToLocal(&done)) {
      return false;
    }
    if (done->BooleanValue(isolate)) {
      return true;
    }
    v8::Local<v8::Value> item;
    if (!result.As<v8::Object>()->Get(context, value_key).ToLocal(&item) ||
        !each(item)) {
      return false;
    }
  }
}

bool IterateSequence(v8::Isolate* isolate,
                     v8::Local<v8::Value> value,
                     ExceptionState& exception_state,
                     base::FunctionRef<bool(v8::Local<v8::Value>)> each) {
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  // As Blink, an array is read by its indices, not through its @@iterator.
  if (value->IsArray()) {
    const v8::Local<v8::Array> array = value.As<v8::Array>();
    for (uint32_t i = 0; i < array->Length(); ++i) {
      v8::Local<v8::Value> item;
      if (!array->Get(context, i).ToLocal(&item) || !each(item)) {
        return false;
      }
    }
    return true;
  }
  if (!value->IsObject()) {
    exception_state.ThrowTypeError(
        "The provided value cannot be converted to a sequence.");
    return false;
  }
  v8::Local<v8::Function> method;
  if (!GetIteratorMethod(isolate, value.As<v8::Object>(), &method)) {
    return false;
  }
  if (method.IsEmpty()) {
    exception_state.ThrowTypeError(
        "The object must have a callable @@iterator property.");
    return false;
  }
  return IterateSequenceWithMethod(isolate, value.As<v8::Object>(), method,
                                   each);
}

bool IterateRecord(
    v8::Isolate* isolate,
    v8::Local<v8::Value> value,
    ExceptionState& exception_state,
    std::string_view type_name,
    base::FunctionRef<bool(v8::Local<v8::Value>, v8::Local<v8::Value>)> each) {
  if (!value->IsObject()) {
    exception_state.ThrowTypeError(
        "Only objects can be converted to record<K,V> types");
    return false;
  }
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::Local<v8::Object> object = value.As<v8::Object>();
  v8::Local<v8::Array> keys;
  if (!object
           ->GetOwnPropertyNames(context,
                                 static_cast<v8::PropertyFilter>(
                                     v8::PropertyFilter::ALL_PROPERTIES),
                                 v8::KeyConversionMode::kConvertToString)
           .ToLocal(&keys)) {
    return false;
  }
  const v8::Local<v8::String> enumerable_key =
      v8::String::NewFromUtf8Literal(isolate, "enumerable");
  const uint32_t length = keys->Length();
  for (uint32_t i = 0; i < length; ++i) {
    v8::Local<v8::Value> key;
    v8::Local<v8::Value> descriptor;
    if (!keys->Get(context, i).ToLocal(&key) ||
        !object->GetOwnPropertyDescriptor(context, key.As<v8::Name>())
             .ToLocal(&descriptor)) {
      return false;
    }
    if (descriptor->IsUndefined()) {
      continue;
    }
    v8::Local<v8::Value> enumerable;
    if (!descriptor.As<v8::Object>()
             ->Get(context, enumerable_key)
             .ToLocal(&enumerable)) {
      return false;
    }
    if (!enumerable->BooleanValue(isolate)) {
      continue;
    }
    v8::Local<v8::Value> item;
    if (!object->Get(context, key).ToLocal(&item) || !each(key, item)) {
      return false;
    }
  }
  return true;
}

v8::Local<v8::String> ToV8ByteString(v8::Isolate* isolate,
                                     std::string_view bytes) {
  return v8::String::NewFromOneByte(
             isolate, reinterpret_cast<const uint8_t*>(bytes.data()),
             v8::NewStringType::kNormal, base::checked_cast<int>(bytes.size()))
      .ToLocalChecked();
}

v8::Local<v8::Value> ToV8NullableByteString(
    v8::Isolate* isolate,
    const std::optional<std::string>& bytes) {
  if (!bytes) {
    return v8::Null(isolate);
  }
  return ToV8ByteString(isolate, *bytes);
}

v8::Local<v8::Value> ToV8ByteStringArray(
    v8::Isolate* isolate,
    const std::vector<std::string>& texts) {
  std::vector<v8::Local<v8::Value>> strings;
  strings.reserve(texts.size());
  for (const std::string& text : texts) {
    strings.push_back(ToV8ByteString(isolate, text));
  }
  return v8::Array::New(isolate, strings.data(), strings.size());
}

ExceptionToRejectPromiseScope::ExceptionToRejectPromiseScope(
    const v8::FunctionCallbackInfo<v8::Value>& info)
    : info_(info), try_catch_(info.GetIsolate()) {}

ExceptionToRejectPromiseScope::~ExceptionToRejectPromiseScope() {
  if (!try_catch_.HasCaught()) {
    return;
  }
  v8::Isolate* const isolate = info_->GetIsolate();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(isolate->GetCurrentContext())
           .ToLocal(&resolver)) {
    return;
  }
  const v8::Local<v8::Value> exception = try_catch_.Exception();
  try_catch_.Reset();
  std::ignore = resolver->Reject(isolate->GetCurrentContext(), exception);
  info_->GetReturnValue().Set(resolver->GetPromise());
}

void ReportException(v8::Isolate* isolate, v8::Local<v8::Value> exception) {
  const PerIsolateData::ExceptionReporter& reporter =
      PerIsolateData::From(isolate)->exception_reporter();
  if (reporter) {
    reporter.Run(exception);
    return;
  }
  const v8::TryCatch try_catch(isolate);
  v8::Local<v8::String> text;
  std::string message = "exception";
  if (exception->ToString(isolate->GetCurrentContext()).ToLocal(&text)) {
    const v8::String::Utf8Value utf8(isolate, text);
    // SAFETY: Utf8Value holds length() bytes at *utf8.
    message.assign(UNSAFE_BUFFERS(std::string_view(*utf8, utf8.length())));
  }
  fprintf(stderr, "Uncaught %s\n", message.c_str());
}

}  // namespace mitto::bindings
