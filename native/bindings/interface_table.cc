#include "mitto/bindings/interface_table.h"

#include <string_view>
#include <tuple>

#include "base/check.h"
#include "base/notreached.h"
#include "mitto/bindings/exception_messages.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/idl_member_installer.h"
#include "mitto/bindings/known_gap.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/runtime_features.h"
#include "mitto/bindings/v8_conversions.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-external.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-template.h"

namespace mitto::bindings {

namespace {

bool IsInstalled(const MemberInfo& member, const PerIsolateData& data) {
  return !member.condition || member.condition(data);
}

v8::Local<v8::External> DataOf(v8::Isolate* isolate, const MemberInfo& member) {
  return v8::External::New(isolate, const_cast<MemberInfo*>(&member),
                           kMemberInfoTag);
}

// The property key of a member's name, a well-known symbol for "@@x".
v8::Local<v8::Name> KeyOf(v8::Isolate* isolate, std::string_view name) {
  if (name == "@@iterator") {
    return v8::Symbol::GetIterator(isolate);
  }
  if (name == "@@asyncIterator") {
    return v8::Symbol::GetAsyncIterator(isolate);
  }
  if (name == "@@unscopables") {
    return v8::Symbol::GetUnscopables(isolate);
  }
  CHECK(!name.starts_with("@@"));
  return ToV8StringFromUtf8(isolate, name);
}

v8::PropertyAttribute AttributesOf(const MemberInfo& member) {
  return static_cast<v8::PropertyAttribute>(member.attributes);
}

bool HasSignature(const MemberInfo& member) {
  return member.target != MemberTarget::kInterface &&
         !(member.flags &
           (kMemberStatic | kMemberLenientThis | kMemberReturnsPromise));
}

// [Replaceable]: bind_gen's make_steps_of_replaceable(), a data property of
// the page's value on the receiver.
void ReplaceableSetter(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* const isolate = info.GetIsolate();
  const auto& member = *static_cast<const MemberInfo*>(
      info.Data().As<v8::External>()->Value(kMemberInfoTag));
  std::ignore = info.This()->CreateDataProperty(
      isolate->GetCurrentContext(), ToV8StringFromUtf8(isolate, member.name),
      info[0]);
}

// [LegacyLenientSetter]: a setter that does nothing.
void LenientSetter(const v8::FunctionCallbackInfo<v8::Value>& info) {}

v8::FunctionCallback SetterOf(const MemberInfo& member) {
  if (!(member.flags & kMemberHasSetter)) {
    return nullptr;
  }
  if (member.flags & kMemberReplaceable) {
    return &ReplaceableSetter;
  }
  if (member.flags & kMemberLenientSetter) {
    return &LenientSetter;
  }
  return &KnownGapAttributeSetter;
}

v8::FunctionCallback OperationCallbackOf(const MemberInfo& member) {
  return (member.flags & kMemberReturnsPromise) ? &KnownGapPromiseOperation
                                                : &KnownGapOperation;
}

void IllegalConstructor(const v8::FunctionCallbackInfo<v8::Value>& info) {
  ThrowTypeError(info.GetIsolate(), "Illegal constructor");
}

// V8HTMLConstructor::HtmlConstructor() with no custom element defined for
// new.target.
void HTMLConstructor(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* const isolate = info.GetIsolate();
  if (!info.IsConstructCall()) {
    ThrowTypeError(isolate, ExceptionMessages::ConstructorCalledAsFunction());
    return;
  }
  ThrowTypeError(isolate, "Illegal constructor");
}

// The templates that a table's targets name.
struct Targets {
  v8::Local<v8::Template> interface_template;
  v8::Local<v8::ObjectTemplate> prototype_template;
  v8::Local<v8::ObjectTemplate> instance_template;
  v8::Local<v8::Signature> signature;

  v8::Local<v8::Template> Of(MemberTarget target) const {
    switch (target) {
      case MemberTarget::kInterface:
        return interface_template;
      case MemberTarget::kPrototype:
        CHECK(!prototype_template.IsEmpty());
        return prototype_template;
      case MemberTarget::kInstance:
        CHECK(!instance_template.IsEmpty());
        return instance_template;
    }
    NOTREACHED();
  }
};

void InstallOnTemplates(v8::Isolate* isolate,
                        const Targets& targets,
                        const InterfaceTable& table,
                        const PerIsolateData& data) {
  for (const MemberInfo& member : table.template_members) {
    if (!IsInstalled(member, data)) {
      continue;
    }
    const v8::Local<v8::Template> target = targets.Of(member.target);
    const v8::Local<v8::Signature> signature =
        HasSignature(member) ? targets.signature : v8::Local<v8::Signature>();
    switch (member.kind) {
      case MemberKind::kAttribute:
        InstallAttribute(isolate, target, signature, member.name,
                         member.owner->interface_name, &KnownGapAttributeGetter,
                         SetterOf(member), AttributesOf(member),
                         v8::SideEffectType::kHasNoSideEffect,
                         DataOf(isolate, member));
        break;
      case MemberKind::kOperation:
        InstallOperation(
            isolate, target, signature, member.name,
            member.owner->interface_name, OperationCallbackOf(member),
            member.length, AttributesOf(member),
            v8::SideEffectType::kHasSideEffect, DataOf(isolate, member));
        break;
      case MemberKind::kConstant:
        InstallConstant(isolate, target, member.name, member.value);
        break;
      case MemberKind::kIntrinsic:
        target->SetIntrinsicDataProperty(
            KeyOf(isolate, member.name),
            static_cast<v8::Intrinsic>(member.value), AttributesOf(member));
        break;
      case MemberKind::kIteratorAlias:
      case MemberKind::kUnscopables:
        // The generator puts these on each context's objects only.
        NOTREACHED();
    }
  }
}

void InstallConstructorOf(v8::Isolate* isolate,
                          v8::Local<v8::FunctionTemplate> interface_template,
                          const WrapperTypeInfo& info,
                          const PerIsolateData& data) {
  const InterfaceTable& table = *info.table;
  const bool has_constructor =
      table.constructor != ConstructorKind::kNone &&
      (!table.constructor_condition || table.constructor_condition(data));
  if (!has_constructor) {
    InstallIllegalConstructor(interface_template, &IllegalConstructor);
    return;
  }
  if (table.constructor == ConstructorKind::kHTMLConstructor) {
    InstallConstructor(interface_template, &HTMLConstructor,
                       table.constructor_length);
    return;
  }
  InstallConstructor(
      interface_template, &KnownGapConstructor, table.constructor_length,
      v8::External::New(isolate, const_cast<WrapperTypeInfo*>(&info),
                        kWrapperTypeInfoTag));
}

// The callback of the operation `name` of the table's template members, for
// HTMLAllCollection's instances, which call item() when called.
v8::FunctionCallback CallbackOfOperation(const InterfaceTable& table,
                                         std::string_view name,
                                         const MemberInfo** found) {
  for (const MemberInfo& member : table.template_members) {
    if (member.kind == MemberKind::kOperation && member.name == name) {
      *found = &member;
      return OperationCallbackOf(member);
    }
  }
  NOTREACHED();
}

void InstallInterface(v8::Isolate* isolate,
                      v8::Local<v8::FunctionTemplate> interface_template,
                      const WrapperTypeInfo& info,
                      const PerIsolateData& data) {
  const InterfaceTable& table = *info.table;
  SetUpInterfaceTemplate(isolate, interface_template, info.parent,
                         info.interface_name);
  const v8::Local<v8::ObjectTemplate> instance_template =
      interface_template->InstanceTemplate();
  const v8::Local<v8::ObjectTemplate> prototype_template =
      interface_template->PrototypeTemplate();
  InstallConstructorOf(isolate, interface_template, info, data);
  if (table.flags & kInterfaceLocation) {
    // Location's specialness, from the HTML standard's "create a Location
    // object".
    instance_template->SetIntrinsicDataProperty(
        ToV8StringFromUtf8(isolate, "valueOf"), v8::kObjProto_valueOf,
        static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontEnum |
                                           v8::DontDelete));
    instance_template->Set(v8::Symbol::GetToPrimitive(isolate),
                           v8::Undefined(isolate),
                           static_cast<v8::PropertyAttribute>(
                               v8::ReadOnly | v8::DontEnum | v8::DontDelete));
    instance_template->SetImmutableProto();
    prototype_template->SetImmutableProto();
  }
  if (table.flags & kInterfaceAllCollection) {
    const MemberInfo* item = nullptr;
    const v8::FunctionCallback callback =
        CallbackOfOperation(table, "item", &item);
    instance_template->SetCallAsFunctionHandler(callback,
                                                DataOf(isolate, *item));
    instance_template->MarkAsUndetectable();
  }
  if ((table.flags & kInterfaceCodeLike) &&
      data.features().IsEnabled(RuntimeFeature::kTrustedTypesUseCodeLike)) {
    instance_template->SetCodeLike();
  }
  if (table.flags & kInterfaceInGlobalPrototypeChain) {
    prototype_template->SetImmutableProto();
  }
  InstallOnTemplates(isolate,
                     {interface_template, prototype_template, instance_template,
                      v8::Signature::New(isolate, interface_template)},
                     table, data);
}

}  // namespace

void InstallTemplateFromTable(v8::Isolate* isolate,
                              v8::Local<v8::Template> construct_template,
                              const WrapperTypeInfo& info) {
  CHECK(info.table);
  const PerIsolateData& data = *PerIsolateData::From(isolate);
  const v8::Local<v8::String> class_string =
      ToV8StringFromUtf8(isolate, info.interface_name);
  switch (info.kind) {
    case WrapperTypeInfo::Kind::kInterface:
      InstallInterface(isolate, construct_template.As<v8::FunctionTemplate>(),
                       info, data);
      return;
    case WrapperTypeInfo::Kind::kCallbackInterface: {
      // SetupIDLCallbackInterfaceTemplate().
      const v8::Local<v8::FunctionTemplate> interface_template =
          construct_template.As<v8::FunctionTemplate>();
      interface_template->RemovePrototype();
      interface_template->SetClassName(class_string);
      interface_template->SetCallHandler(&IllegalConstructor);
      InstallOnTemplates(isolate, {interface_template, {}, {}, {}}, *info.table,
                         data);
      return;
    }
    case WrapperTypeInfo::Kind::kNamespace: {
      // SetupIDLNamespaceTemplate().
      const v8::Local<v8::ObjectTemplate> namespace_template =
          construct_template.As<v8::ObjectTemplate>();
      namespace_template->Set(
          v8::Symbol::GetToStringTag(isolate), class_string,
          static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontEnum));
      InstallOnTemplates(isolate, {namespace_template, {}, {}, {}}, *info.table,
                         data);
      return;
    }
  }
  NOTREACHED();
}

void InstallContextPropertiesFromTable(v8::Local<v8::Context> context,
                                       v8::Local<v8::Object> interface_object,
                                       v8::Local<v8::Object> prototype_object,
                                       v8::Local<v8::Object> instance_object,
                                       const WrapperTypeInfo& info) {
  CHECK(info.table);
  const InterfaceTable& table = *info.table;
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const PerIsolateData& data = *PerIsolateData::From(isolate);
  if (table.flags & kInterfaceNoInterfaceObject) {
    // V8 defines `constructor` on every prototype that a template makes.
    DeleteConstructorProperty(isolate, context, prototype_object);
  }
  v8::Local<v8::Signature> signature;
  if (info.kind == WrapperTypeInfo::Kind::kInterface) {
    signature = v8::Signature::New(
        isolate,
        PerIsolateData::From(isolate)->InterfaceTemplate(isolate, &info));
  }
  for (const MemberInfo& member : table.context_members) {
    if (!IsInstalled(member, data)) {
      continue;
    }
    // A table's interface has no global object, so it installs nothing on an
    // instance for each context.
    CHECK(member.target != MemberTarget::kInstance);
    const v8::Local<v8::Object> target =
        member.target == MemberTarget::kInterface ? interface_object
                                                  : prototype_object;
    const v8::Local<v8::Signature> member_signature =
        HasSignature(member) ? signature : v8::Local<v8::Signature>();
    switch (member.kind) {
      case MemberKind::kAttribute:
        InstallAttributeOnObject(
            isolate, context, target, member_signature, member.name,
            info.interface_name, &KnownGapAttributeGetter, SetterOf(member),
            AttributesOf(member), v8::SideEffectType::kHasNoSideEffect,
            DataOf(isolate, member));
        break;
      case MemberKind::kOperation:
        InstallOperationOnObject(
            isolate, context, target, member_signature, member.name,
            info.interface_name, OperationCallbackOf(member), member.length,
            AttributesOf(member), v8::SideEffectType::kHasSideEffect,
            DataOf(isolate, member));
        break;
      case MemberKind::kConstant:
        InstallConstantOnObject(isolate, context, target, member.name,
                                member.value);
        break;
      case MemberKind::kIteratorAlias: {
        // Blink's _make_install_prototype_object(): the same function as the
        // operation it is, such as entries().
        v8::Local<v8::Value> function;
        if (prototype_object
                ->Get(context, ToV8StringFromUtf8(isolate, member.forward))
                .ToLocal(&function)) {
          std::ignore = prototype_object->DefineOwnProperty(
              context, KeyOf(isolate, member.name), function,
              AttributesOf(member));
        }
        break;
      }
      case MemberKind::kUnscopables:
        InstallUnscopablePropertyNames(isolate, context, prototype_object,
                                       table.unscopables);
        break;
      case MemberKind::kIntrinsic:
        // The generator puts these on templates only.
        NOTREACHED();
    }
  }
}

}  // namespace mitto::bindings
