#include "mitto/bindings/v8_conversions.h"

#include <stdint.h>
#include <stdio.h>

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
