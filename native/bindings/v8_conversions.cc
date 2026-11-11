#include "mitto/bindings/v8_conversions.h"

#include <stdint.h>
#include <stdio.h>

#include <utility>

#include "base/compiler_specific.h"
#include "base/numerics/safe_conversions.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"

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

void ReportException(v8::Isolate* isolate, v8::Local<v8::Value> exception) {
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
