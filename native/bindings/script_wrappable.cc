#include "mitto/bindings/script_wrappable.h"

#include <tuple>

#include "base/check.h"
#include "mitto/bindings/idl_member_installer.h"
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
  // constructor, as Blink makes the wrapper of an object that C++ created,
  // once the context has made the interface object, as Blink's
  // V8DOMWrapper::CreateWrapper() asks for it first, so that the prototype
  // has what the context decides.
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  std::ignore = InterfaceObject(context, *GetWrapperTypeInfo());
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

void ScriptWrappable::AssociateWithGlobalProxy(
    v8::Isolate* isolate,
    v8::Local<v8::Object> global_proxy) {
  CHECK(wrapper_.IsEmpty());
  v8::Object::WrapGlobal(isolate, global_proxy, this,
                         GetWrapperTypeInfo()->tag);
  wrapper_.Reset(isolate, global_proxy);
}

}  // namespace mitto::bindings
