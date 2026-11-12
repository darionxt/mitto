#include "mitto/bindings/idl_member_installer.h"

#include <string>
#include <tuple>

#include "base/check.h"
#include "base/check_op.h"
#include "base/notreached.h"
#include "base/strings/strcat.h"
#include "mitto/bindings/interface_table.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/v8_conversions.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-external.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-promise.h"

namespace mitto::bindings {

namespace {

// The context's embedder data that holds the objects it made for constructs.
// Index 0 is Chrome's debugger's (v8-context.h).
constexpr int kContextObjectsIndex = 1;

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
    v8::ExceptionContext exception_context,
    v8::Local<v8::Value> data) {
  const v8::Local<v8::FunctionTemplate> function =
      v8::FunctionTemplate::New(isolate, callback, data, signature, length,
                                v8::ConstructorBehavior::kThrow, side_effect);
  function->SetClassName(ToV8StringFromUtf8(isolate, function_name));
  function->SetInterfaceName(ToV8StringFromUtf8(isolate, interface_name));
  function->SetExceptionContext(exception_context);
  return function;
}

// A constant's value, as IDLMemberInstaller::InstallConstants() makes it.
v8::Local<v8::Integer> ConstantValue(v8::Isolate* isolate, int64_t value) {
  if (value < 0) {
    return v8::Integer::New(isolate, base::checked_cast<int32_t>(value));
  }
  return v8::Integer::NewFromUnsigned(isolate,
                                      base::checked_cast<uint32_t>(value));
}

// The context's map from WrapperTypeInfo::index to the interface objects and
// factory functions it made, as Blink's V8PerContextData keeps them, which it
// makes the first time.
v8::Local<v8::Map> ObjectsOfContext(v8::Local<v8::Context> context) {
  if (context->GetNumberOfEmbedderDataFields() >
      static_cast<uint32_t>(kContextObjectsIndex)) {
    const v8::Local<v8::Data> data =
        context->GetEmbedderDataV2(kContextObjectsIndex);
    if (data->IsValue() && data.As<v8::Value>()->IsMap()) {
      return data.As<v8::Map>();
    }
  }
  const v8::Local<v8::Map> objects = v8::Map::New(v8::Isolate::GetCurrent());
  context->SetEmbedderDataV2(kContextObjectsIndex, objects);
  return objects;
}

// The key of a construct's object in ObjectsOfContext(): its index, and for a
// factory function the index after all of them, by its name.
v8::Local<v8::Value> KeyOfObject(v8::Isolate* isolate,
                                 const WrapperTypeInfo& info) {
  return v8::Integer::NewFromUnsigned(isolate, info.index);
}

// The constructor of a [LegacyFactoryFunction], as Blink's
// CreateLegacyFactoryFunctionFunction() makes it: it inherits from the
// interface's template, so that it makes objects of the interface, and its
// `prototype` is the interface's prototype object.
v8::Local<v8::Function> FactoryFunction(v8::Local<v8::Context> context,
                                        const ExposedConstructInfo& construct) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Map> objects = ObjectsOfContext(context);
  const v8::Local<v8::String> key = ToV8StringFromUtf8(isolate, construct.name);
  v8::Local<v8::Value> cached;
  if (objects->Get(context, key).ToLocal(&cached) && cached->IsFunction()) {
    return cached.As<v8::Function>();
  }
  PerIsolateData* const data = PerIsolateData::From(isolate);
  const v8::Local<v8::FunctionTemplate> function_template =
      data->FactoryFunctionTemplate(isolate, construct);
  const v8::Local<v8::Function> function =
      function_template->GetFunction(context).ToLocalChecked();
  const v8::Local<v8::Value> prototype =
      InterfaceObject(context, *construct.target)
          ->Get(context, v8::String::NewFromUtf8Literal(isolate, "prototype"))
          .ToLocalChecked();
  CHECK(function
            ->DefineOwnProperty(
                context, v8::String::NewFromUtf8Literal(isolate, "prototype"),
                prototype,
                static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontEnum |
                                                   v8::DontDelete))
            .FromJust());
  CHECK(!objects->Set(context, key, function).IsEmpty());
  return function;
}

// The getter of a lazy data property that gives an exposed construct, as
// Blink's exposed construct callbacks.
void ExposedConstructGetter(v8::Local<v8::Name> name,
                            const v8::PropertyCallbackInfo<v8::Value>& info) {
  v8::Isolate* const isolate = info.GetIsolate();
  const auto& construct = *static_cast<const ExposedConstructInfo*>(
      info.Data().As<v8::External>()->Value(kExposedConstructInfoTag));
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  switch (construct.kind) {
    case ExposedConstructInfo::Kind::kInterfaceObject:
      info.GetReturnValue().Set(InterfaceObject(context, *construct.target));
      return;
    case ExposedConstructInfo::Kind::kNamespaceObject:
      info.GetReturnValue().Set(NamespaceObject(context, *construct.target));
      return;
    case ExposedConstructInfo::Kind::kFactoryFunction:
      info.GetReturnValue().Set(FactoryFunction(context, construct));
      return;
  }
  NOTREACHED();
}

v8::Local<v8::External> DataOfConstruct(v8::Isolate* isolate,
                                        const ExposedConstructInfo& construct) {
  return v8::External::New(isolate,
                           const_cast<ExposedConstructInfo*>(&construct),
                           kExposedConstructInfoTag);
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
                        int length,
                        v8::Local<v8::Value> data) {
  interface_template->SetCallHandler(callback, data);
  interface_template->SetLength(length);
  interface_template->SetExceptionContext(v8::ExceptionContext::kConstructor);
}

void InstallIllegalConstructor(
    v8::Local<v8::FunctionTemplate> interface_template,
    v8::FunctionCallback callback) {
  interface_template->SetCallHandler(callback);
  interface_template->SetLength(0);
}

void InstallAttribute(v8::Isolate* isolate,
                      v8::Local<v8::Template> target,
                      v8::Local<v8::Signature> signature,
                      const char* name,
                      const char* interface_name,
                      v8::FunctionCallback getter,
                      v8::FunctionCallback setter,
                      v8::PropertyAttribute attributes,
                      v8::SideEffectType getter_side_effect,
                      v8::Local<v8::Value> data) {
  const v8::Local<v8::FunctionTemplate> getter_function = MemberFunction(
      isolate, getter, signature, base::StrCat({"get ", name}), interface_name,
      0, getter_side_effect, v8::ExceptionContext::kAttributeGet, data);
  v8::Local<v8::FunctionTemplate> setter_function;
  if (setter) {
    setter_function =
        MemberFunction(isolate, setter, signature, base::StrCat({"set ", name}),
                       interface_name, 1, v8::SideEffectType::kHasSideEffect,
                       v8::ExceptionContext::kAttributeSet, data);
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
                      v8::SideEffectType side_effect,
                      v8::Local<v8::Value> data) {
  target->Set(
      ToV8StringFromUtf8(isolate, name),
      MemberFunction(isolate, callback, signature, name, interface_name, length,
                     side_effect, v8::ExceptionContext::kOperation, data),
      attributes);
}

void InstallAttributeOnObject(v8::Isolate* isolate,
                              v8::Local<v8::Context> context,
                              v8::Local<v8::Object> target,
                              v8::Local<v8::Signature> signature,
                              const char* name,
                              const char* interface_name,
                              v8::FunctionCallback getter,
                              v8::FunctionCallback setter,
                              v8::PropertyAttribute attributes,
                              v8::SideEffectType getter_side_effect,
                              v8::Local<v8::Value> data) {
  const v8::Local<v8::Function> getter_function =
      MemberFunction(isolate, getter, signature, base::StrCat({"get ", name}),
                     interface_name, 0, getter_side_effect,
                     v8::ExceptionContext::kAttributeGet, data)
          ->GetFunction(context)
          .ToLocalChecked();
  v8::Local<v8::Function> setter_function;
  if (setter) {
    setter_function =
        MemberFunction(isolate, setter, signature, base::StrCat({"set ", name}),
                       interface_name, 1, v8::SideEffectType::kHasSideEffect,
                       v8::ExceptionContext::kAttributeSet, data)
            ->GetFunction(context)
            .ToLocalChecked();
  }
  target->SetAccessorProperty(ToV8StringFromUtf8(isolate, name),
                              getter_function, setter_function, attributes);
}

void InstallOperationOnObject(v8::Isolate* isolate,
                              v8::Local<v8::Context> context,
                              v8::Local<v8::Object> target,
                              v8::Local<v8::Signature> signature,
                              const char* name,
                              const char* interface_name,
                              v8::FunctionCallback callback,
                              int length,
                              v8::PropertyAttribute attributes,
                              v8::SideEffectType side_effect,
                              v8::Local<v8::Value> data) {
  const v8::Local<v8::Function> function =
      MemberFunction(isolate, callback, signature, name, interface_name, length,
                     side_effect, v8::ExceptionContext::kOperation, data)
          ->GetFunction(context)
          .ToLocalChecked();
  CHECK(target
            ->DefineOwnProperty(context, ToV8StringFromUtf8(isolate, name),
                                function, attributes)
            .FromJust());
}

void InstallUnscopablePropertyNames(
    v8::Isolate* isolate,
    v8::Local<v8::Context> context,
    v8::Local<v8::Object> prototype_object,
    base::span<const char* const> property_names) {
  const v8::Local<v8::Object> unscopables =
      v8::Object::New(isolate, v8::Null(isolate), nullptr, nullptr, 0);
  for (const char* const name : property_names) {
    CHECK(unscopables
              ->CreateDataProperty(context, ToV8StringFromUtf8(isolate, name),
                                   v8::True(isolate))
              .FromJust());
  }
  CHECK(prototype_object
            ->DefineOwnProperty(
                context, v8::Symbol::GetUnscopables(isolate), unscopables,
                static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontEnum))
            .FromJust());
}

void DeleteConstructorProperty(v8::Isolate* isolate,
                               v8::Local<v8::Context> context,
                               v8::Local<v8::Object> prototype_object) {
  CHECK(prototype_object
            ->Delete(context,
                     v8::String::NewFromUtf8Literal(isolate, "constructor"))
            .FromJust());
}

void InstallConstant(v8::Isolate* isolate,
                     v8::Local<v8::Template> target,
                     const char* name,
                     int64_t value) {
  target->Set(
      ToV8StringFromUtf8(isolate, name), ConstantValue(isolate, value),
      static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontDelete));
}

void InstallConstantOnObject(v8::Isolate* isolate,
                             v8::Local<v8::Context> context,
                             v8::Local<v8::Object> target,
                             const char* name,
                             int64_t value) {
  CHECK(target
            ->DefineOwnProperty(context, ToV8StringFromUtf8(isolate, name),
                                ConstantValue(isolate, value),
                                static_cast<v8::PropertyAttribute>(
                                    v8::ReadOnly | v8::DontDelete))
            .FromJust());
}

v8::Local<v8::Function> InterfaceObject(v8::Local<v8::Context> context,
                                        const WrapperTypeInfo& info) {
  CHECK(info.kind != WrapperTypeInfo::Kind::kNamespace);
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Map> objects = ObjectsOfContext(context);
  const v8::Local<v8::Value> key = KeyOfObject(isolate, info);
  v8::Local<v8::Value> cached;
  if (objects->Get(context, key).ToLocal(&cached) && cached->IsFunction()) {
    return cached.As<v8::Function>();
  }
  // As V8PerContextData::ConstructorForType(): the parent's first, also one
  // that has no interface object, such as WindowProperties, so that what the
  // context decides is installed on its prototype too.
  if (info.parent) {
    std::ignore = InterfaceObject(context, *info.parent);
  }
  const v8::Local<v8::Function> interface_object =
      PerIsolateData::From(isolate)
          ->InterfaceTemplate(isolate, &info)
          ->GetFunction(context)
          .ToLocalChecked();
  // As V8ObjectConstructor::CreateInterfaceObject(): the template links the
  // prototypes, and the interface object of an interface that inherits gets
  // its parent's for its own prototype here.
  if (info.interface_object_parent) {
    CHECK(interface_object
              ->SetPrototype(
                  context,
                  InterfaceObject(context, *info.interface_object_parent))
              .FromJust());
  }
  CHECK(!objects->Set(context, key, interface_object).IsEmpty());
  v8::Local<v8::Object> prototype_object;
  if (info.kind == WrapperTypeInfo::Kind::kInterface) {
    prototype_object =
        interface_object
            ->Get(context, v8::String::NewFromUtf8Literal(isolate, "prototype"))
            .ToLocalChecked()
            .As<v8::Object>();
  }
  info.install_context_dependent_properties(context, interface_object,
                                            prototype_object,
                                            v8::Local<v8::Object>(), info);
  return interface_object;
}

v8::Local<v8::Object> NamespaceObject(v8::Local<v8::Context> context,
                                      const WrapperTypeInfo& info) {
  CHECK(info.kind == WrapperTypeInfo::Kind::kNamespace);
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Object> namespace_object =
      PerIsolateData::From(isolate)
          ->NamespaceTemplate(isolate, &info)
          ->NewInstance(context)
          .ToLocalChecked();
  info.install_context_dependent_properties(context, namespace_object,
                                            v8::Local<v8::Object>(),
                                            v8::Local<v8::Object>(), info);
  return namespace_object;
}

void RejectWithTypeError(const v8::FunctionCallbackInfo<v8::Value>& info,
                         std::string_view message) {
  v8::Isolate* const isolate = info.GetIsolate();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return;
  }
  std::ignore = resolver->Reject(
      context, v8::Exception::TypeError(ToV8StringFromUtf8(isolate, message)));
  info.GetReturnValue().Set(resolver->GetPromise());
}

void InstallExposedConstruct(v8::Isolate* isolate,
                             v8::Local<v8::ObjectTemplate> instance_template,
                             const ExposedConstructInfo& construct) {
  instance_template->SetLazyDataProperty(
      ToV8StringFromUtf8(isolate, construct.name), &ExposedConstructGetter,
      DataOfConstruct(isolate, construct), v8::DontEnum,
      v8::SideEffectType::kHasNoSideEffect);
}

void InstallExposedConstructOnObject(v8::Isolate* isolate,
                                     v8::Local<v8::Context> context,
                                     v8::Local<v8::Object> instance_object,
                                     const ExposedConstructInfo& construct) {
  CHECK(instance_object
            ->SetLazyDataProperty(
                context, ToV8StringFromUtf8(isolate, construct.name),
                &ExposedConstructGetter, DataOfConstruct(isolate, construct),
                v8::DontEnum, v8::SideEffectType::kHasNoSideEffect)
            .FromJust());
}

}  // namespace mitto::bindings
