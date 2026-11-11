#include "mitto/bindings/exception_context.h"

#include <string>
#include <tuple>

#include "base/compiler_specific.h"
#include "mitto/bindings/exception_messages.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/v8_conversions.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"

namespace mitto::bindings {

namespace {

std::string ToUtf8(v8::Isolate* isolate, v8::Local<v8::String> text) {
  const v8::String::Utf8Value utf8(isolate, text);
  // SAFETY: Utf8Value holds length() bytes at *utf8.
  return std::string(UNSAFE_BUFFERS(std::string_view(*utf8, utf8.length())));
}

// Blink's ApplyContextToException(): a DOMException gets the context in its
// message, and any other object in its own `message` property.
void ApplyContextToException(v8::Isolate* isolate,
                             v8::Local<v8::Object> exception,
                             v8::ExceptionContext context_type,
                             std::string_view class_name,
                             std::string_view property_name) {
  const PerIsolateData::DOMExceptionHooks& hooks =
      PerIsolateData::From(isolate)->dom_exception_hooks();
  if (hooks.add_context && hooks.add_context(isolate, exception, context_type,
                                             class_name, property_name)) {
    return;
  }
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::TryCatch try_catch(isolate);
  const v8::Local<v8::String> message_key =
      v8::String::NewFromUtf8Literal(isolate, "message");
  v8::Local<v8::Value> message;
  v8::Local<v8::String> message_string;
  if (!exception->Get(context, message_key).ToLocal(&message) ||
      !message->ToString(context).ToLocal(&message_string)) {
    return;
  }
  const std::string updated = ExceptionMessages::AddContextToMessage(
      context_type, class_name, property_name, ToUtf8(isolate, message_string));
  std::ignore = exception->CreateDataProperty(
      context, message_key, ToV8StringFromUtf8(isolate, updated));
}

void OnExceptionPropagation(v8::ExceptionPropagationMessage message) {
  v8::Isolate* const isolate = message.GetIsolate();
  const v8::Local<v8::Object> exception = message.GetException();
  const v8::ExceptionContext context_type = message.GetExceptionContext();
  std::string class_name = ToUtf8(isolate, message.GetInterfaceName());
  std::string property_name = ToUtf8(isolate, message.GetPropertyName());
  // An attribute's functions are named "get x" and "set x".
  if ((context_type == v8::ExceptionContext::kAttributeGet &&
       property_name.starts_with("get ")) ||
      (context_type == v8::ExceptionContext::kAttributeSet &&
       property_name.starts_with("set "))) {
    property_name.erase(0, 4);
  }
  if (property_name == "[Symbol.toPrimitive]") {
    property_name.clear();
  }
  // V8 gives a constructor's name as its property, and the context names it
  // as the class.
  if (context_type == v8::ExceptionContext::kConstructor) {
    class_name = property_name;
  }
  // The innermost dictionary first, so that its context ends up nearest the
  // message.
  for (const PerIsolateData::DictionaryContext& dictionary :
       PerIsolateData::From(isolate)->dictionary_contexts_innermost_first()) {
    ApplyContextToException(
        isolate, exception, v8::ExceptionContext::kAttributeGet,
        dictionary.dictionary_name, dictionary.property_name);
  }
  ApplyContextToException(isolate, exception, context_type, class_name,
                          property_name);
}

}  // namespace

void SetUpExceptionContext(v8::Isolate* isolate) {
  isolate->SetExceptionPropagationCallback(&OnExceptionPropagation);
}

DictionaryConversionScope::DictionaryConversionScope(
    v8::Isolate* isolate,
    std::string_view dictionary_name)
    : data_(PerIsolateData::From(isolate)) {
  data_->PushDictionaryContext(dictionary_name);
}

DictionaryConversionScope::~DictionaryConversionScope() {
  data_->PopDictionaryContext();
}

void DictionaryConversionScope::SetCurrentPropertyName(
    std::string_view property_name) {
  data_->SetDictionaryPropertyName(property_name);
}

}  // namespace mitto::bindings
