#include "mitto/bindings/idl_member_installer.h"

#include <string>

#include "base/check.h"
#include "base/check_op.h"
#include "base/strings/strcat.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/v8_conversions.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto::bindings {

namespace {

// A member's function, as IDLMemberInstaller's CreateFunctionTemplate() makes
// it: not a constructor, with the member's name for its own.
v8::Local<v8::FunctionTemplate> MemberFunction(
    v8::Isolate* isolate,
    v8::FunctionCallback callback,
    v8::Local<v8::Signature> signature,
    std::string_view function_name,
    const char* interface_name,
    int length,
    v8::SideEffectType side_effect,
    v8::ExceptionContext exception_context) {
  const v8::Local<v8::FunctionTemplate> function = v8::FunctionTemplate::New(
      isolate, callback, v8::Local<v8::Value>(), signature, length,
      v8::ConstructorBehavior::kThrow, side_effect);
  function->SetClassName(ToV8StringFromUtf8(isolate, function_name));
  function->SetInterfaceName(ToV8StringFromUtf8(isolate, interface_name));
  function->SetExceptionContext(exception_context);
  return function;
}

}  // namespace

void SetUpInterfaceTemplate(v8::Isolate* isolate,
                            v8::Local<v8::FunctionTemplate> interface_template,
                            const WrapperTypeInfo* parent,
                            const char* interface_name) {
  if (parent) {
    interface_template->Inherit(
        PerIsolateData::From(isolate)->InterfaceTemplate(isolate, parent));
  }
  interface_template->ReadOnlyPrototype();
  const v8::Local<v8::String> class_string =
      ToV8StringFromUtf8(isolate, interface_name);
  interface_template->SetClassName(class_string);
  interface_template->PrototypeTemplate()->Set(
      v8::Symbol::GetToStringTag(isolate), class_string,
      static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontEnum));
}

void InheritFromErrorPrototype(
    v8::Isolate* isolate,
    v8::Local<v8::FunctionTemplate> interface_template) {
  const v8::Local<v8::FunctionTemplate> error_template =
      v8::FunctionTemplate::New(isolate, nullptr, v8::Local<v8::Value>(),
                                v8::Local<v8::Signature>(), 0,
                                v8::ConstructorBehavior::kThrow);
  error_template->SetIntrinsicDataProperty(
      ToV8StringFromUtf8(isolate, "prototype"), v8::kErrorPrototype);
  interface_template->Inherit(error_template);
}

void InstallConstructor(v8::Local<v8::FunctionTemplate> interface_template,
                        v8::FunctionCallback callback,
                        int length) {
  interface_template->SetCallHandler(callback);
  interface_template->SetLength(length);
  interface_template->SetExceptionContext(v8::ExceptionContext::kConstructor);
}

void InstallAttribute(v8::Isolate* isolate,
                      v8::Local<v8::Template> target,
                      v8::Local<v8::Signature> signature,
                      const char* name,
                      const char* interface_name,
                      v8::FunctionCallback getter,
                      v8::FunctionCallback setter,
                      v8::PropertyAttribute attributes,
                      v8::SideEffectType getter_side_effect) {
  const v8::Local<v8::FunctionTemplate> getter_function = MemberFunction(
      isolate, getter, signature, base::StrCat({"get ", name}), interface_name,
      0, getter_side_effect, v8::ExceptionContext::kAttributeGet);
  v8::Local<v8::FunctionTemplate> setter_function;
  if (setter) {
    setter_function =
        MemberFunction(isolate, setter, signature, base::StrCat({"set ", name}),
                       interface_name, 1, v8::SideEffectType::kHasSideEffect,
                       v8::ExceptionContext::kAttributeSet);
  }
  target->SetAccessorProperty(ToV8StringFromUtf8(isolate, name),
                              getter_function, setter_function, attributes);
}

void InstallOperation(v8::Isolate* isolate,
                      v8::Local<v8::Template> target,
                      v8::Local<v8::Signature> signature,
                      const char* name,
                      const char* interface_name,
                      v8::FunctionCallback callback,
                      int length,
                      v8::PropertyAttribute attributes,
                      v8::SideEffectType side_effect) {
  target->Set(
      ToV8StringFromUtf8(isolate, name),
      MemberFunction(isolate, callback, signature, name, interface_name, length,
                     side_effect, v8::ExceptionContext::kOperation),
      attributes);
}

void InstallConstant(v8::Isolate* isolate,
                     v8::Local<v8::Template> target,
                     const char* name,
                     int64_t value) {
  v8::Local<v8::Integer> integer;
  if (value < 0) {
    integer = v8::Integer::New(isolate, base::checked_cast<int32_t>(value));
  } else {
    integer = v8::Integer::NewFromUnsigned(isolate,
                                           base::checked_cast<uint32_t>(value));
  }
  target->Set(
      ToV8StringFromUtf8(isolate, name), integer,
      static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontDelete));
}

v8::Local<v8::Function> InterfaceObject(v8::Local<v8::Context> context,
                                        const WrapperTypeInfo& info) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Function> interface_object =
      PerIsolateData::From(isolate)
          ->InterfaceTemplate(isolate, &info)
          ->GetFunction(context)
          .ToLocalChecked();
  // As Blink's V8ObjectConstructor::CreateInterfaceObject() does: the template
  // links the prototypes, and the interface object of an interface that
  // inherits gets its parent's for its own prototype here.
  if (info.parent) {
    CHECK(interface_object
              ->SetPrototype(context, InterfaceObject(context, *info.parent))
              .FromJust());
  }
  return interface_object;
}

void InstallInterfaceObject(v8::Local<v8::Context> context,
                            const char* name,
                            const WrapperTypeInfo& info) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Function> interface_object =
      InterfaceObject(context, info);
  CHECK(context->Global()
            ->DefineOwnProperty(context, ToV8StringFromUtf8(isolate, name),
                                interface_object, v8::DontEnum)
            .FromJust());
}

}  // namespace mitto::bindings
