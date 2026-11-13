#include "mitto/dom/error_event.h"

#include "mitto/bindings/v8_error_event.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

namespace {

bindings::EventInit ScriptErrorInit() {
  bindings::EventInit init;
  init.cancelable = true;
  return init;
}

}  // namespace

// static
ErrorEvent* ErrorEvent::Create(const std::u16string& type,
                               const bindings::ErrorEventInit& init) {
  return bindings::MakeGarbageCollected<ErrorEvent>(type, init);
}

// static
ErrorEvent* ErrorEvent::CreateForScriptError(const std::u16string& message,
                                             const std::u16string& filename,
                                             uint32_t lineno,
                                             uint32_t colno,
                                             v8::Local<v8::Value> error) {
  return bindings::MakeGarbageCollected<ErrorEvent>(message, filename, lineno,
                                                    colno, error);
}

// static
ErrorEvent* ErrorEvent::CreateSanitized() {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  return CreateForScriptError(u"Script error.", u"", 0, 0, v8::Null(isolate));
}

ErrorEvent::ErrorEvent(const std::u16string& type,
                       const bindings::ErrorEventInit& init)
    : Event(type, init),
      message_(init.message),
      filename_(init.filename),
      lineno_(init.lineno),
      colno_(init.colno) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  // Blink keeps undefined when the init has no error, and nothing, which
  // reads as null, for one that is null.
  if (!init.error) {
    error_.Reset(isolate, v8::Undefined(isolate));
  } else if (!init.error->IsEmpty() && !(*init.error)->IsNull()) {
    error_.Reset(isolate, *init.error);
  }
}

ErrorEvent::ErrorEvent(const std::u16string& message,
                       const std::u16string& filename,
                       uint32_t lineno,
                       uint32_t colno,
                       v8::Local<v8::Value> error)
    : Event(u"error", ScriptErrorInit()),
      message_(message),
      filename_(filename),
      lineno_(lineno),
      colno_(colno) {
  if (!error.IsEmpty()) {
    error_.Reset(v8::Isolate::GetCurrent(), error);
  }
}

ErrorEvent::~ErrorEvent() = default;

const bindings::WrapperTypeInfo* ErrorEvent::GetWrapperTypeInfo() const {
  return &bindings::V8ErrorEvent::kWrapperTypeInfo;
}

void ErrorEvent::Trace(cppgc::Visitor* visitor) const {
  Event::Trace(visitor);
  visitor->Trace(error_);
}

bool ErrorEvent::IsErrorEvent() const {
  return true;
}

v8::Local<v8::Value> ErrorEvent::Error() const {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (error_.IsEmpty()) {
    return v8::Null(isolate);
  }
  return error_.Get(isolate);
}

}  // namespace mitto
