#include "mitto/bindings/script_wrappable.h"

#include "base/check.h"
#include "mitto/bindings/per_isolate_data.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-template.h"

namespace mitto::bindings {

void ScriptWrappable::Trace(cppgc::Visitor* visitor) const {
  v8::Object::Wrappable::Trace(visitor);
  visitor->Trace(wrapper_);
}

v8::Local<v8::Object> ScriptWrappable::ToV8(v8::Isolate* isolate) {
  if (!wrapper_.IsEmpty()) {
    return wrapper_.Get(isolate);
  }
  // An instance of the interface's template, made without calling its
  // constructor, as Blink makes the wrapper of an object that C++ created.
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::Local<v8::FunctionTemplate> interface_template =
      PerIsolateData::From(isolate)->InterfaceTemplate(isolate,
                                                       GetWrapperTypeInfo());
  const v8::Local<v8::Object> wrapper = interface_template->InstanceTemplate()
                                            ->NewInstance(context)
                                            .ToLocalChecked();
  AssociateWithWrapper(isolate, wrapper);
  return wrapper;
}

void ScriptWrappable::AssociateWithWrapper(v8::Isolate* isolate,
                                           v8::Local<v8::Object> wrapper) {
  CHECK(wrapper_.IsEmpty());
  v8::Object::Wrap(isolate, wrapper, this, GetWrapperTypeInfo()->tag);
  wrapper_.Reset(isolate, wrapper);
}

}  // namespace mitto::bindings
