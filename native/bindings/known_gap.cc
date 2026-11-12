#include "mitto/bindings/known_gap.h"

#include <stdint.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "base/check.h"
#include "base/notreached.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "mitto/bindings/exception_messages.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/idl_conversions.h"
#include "mitto/bindings/idl_member_installer.h"
#include "mitto/bindings/interface_table.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-external.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-template.h"
#include "v8/include/v8-typed-array.h"

// Upstream: third_party/blink/renderer/bindings/scripts/bind_gen/interface.py
// Upstream: third_party/blink/renderer/core/frame/use_counter_impl.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/js_event_handler.cc

namespace mitto::bindings {

namespace {

const MemberInfo& MemberOf(const v8::FunctionCallbackInfo<v8::Value>& info) {
  return *static_cast<const MemberInfo*>(
      info.Data().As<v8::External>()->Value(kMemberInfoTag));
}

bool IsObjectOf(v8::Isolate* isolate,
                const WrapperTypeInfo& info,
                v8::Local<v8::Value> value) {
  return PerIsolateData::From(isolate)->HasInstance(isolate, info, value);
}

// The member as the listing names it, such as "Navigator.prototype.getBattery".
std::string NameOf(const MemberInfo& member) {
  const char* owner_part = ".";
  if (member.target == MemberTarget::kPrototype) {
    owner_part = ".prototype.";
  } else if (member.target == MemberTarget::kInstance) {
    owner_part = " instance.";
  }
  return base::StrCat({member.owner->interface_name, owner_part, member.name});
}

void Report(v8::Isolate* isolate, uint16_t number, std::string_view name) {
  PerIsolateData::From(isolate)->ReportKnownGap(number, name);
}

void ReportMember(v8::Isolate* isolate, const MemberInfo& member) {
  if (member.flags & kMemberEventHandler) {
    return;
  }
  Report(isolate, member.number, NameOf(member));
}

void ThrowNotImplemented(v8::Isolate* isolate) {
  ExceptionState exception_state(isolate);
  exception_state.ThrowTypeError(ExceptionMessages::NotImplemented());
}

v8::Local<v8::Object> NewPlainObject(v8::Isolate* isolate) {
  return v8::Object::New(isolate);
}

v8::MaybeLocal<v8::Value> DefaultOf(v8::Isolate* isolate,
                                    v8::Local<v8::Context> context,
                                    const IdlTypeInfo& type);

// Whether the type has a default, as known_gaps.py's default_is_safe() says.
bool HasDefault(const IdlTypeInfo& type) {
  if (type.flags & kIdlTypeNullable) {
    return true;
  }
  switch (type.kind) {
    case IdlTypeKind::kInterface:
      return type.interface && IsSafeObjectType(*type.interface);
    case IdlTypeKind::kUnion:
      for (const IdlTypeInfo* member : type.members) {
        if (HasDefault(*member)) {
          return true;
        }
      }
      return false;
    case IdlTypeKind::kPromise:
      return HasDefault(*type.members[0]);
    case IdlTypeKind::kIterator:
    case IdlTypeKind::kSymbol:
    case IdlTypeKind::kCallbackFunction:
    case IdlTypeKind::kCallbackInterface:
      return false;
    default:
      return true;
  }
}

// A dictionary as Blink gives it to a page when no member is given: an object
// with each member that has a default, the inherited ones first.
v8::MaybeLocal<v8::Value> DefaultDictionary(v8::Isolate* isolate,
                                            v8::Local<v8::Context> context,
                                            const DictionaryInfo& dictionary,
                                            v8::Local<v8::Object> object);

// A dictionary member's default, as the IDL file writes it.
v8::MaybeLocal<v8::Value> DefaultFromLiteral(
    v8::Isolate* isolate,
    v8::Local<v8::Context> context,
    const DictionaryMemberInfo& member) {
  const std::string_view literal = member.default_value;
  if (literal == "true" || literal == "false") {
    return v8::Boolean::New(isolate, literal == "true");
  }
  if (literal == "null") {
    return v8::Null(isolate);
  }
  if (literal == "[]") {
    return v8::Array::New(isolate, 0);
  }
  if (literal == "{}") {
    if (member.type->kind == IdlTypeKind::kDictionary) {
      return DefaultDictionary(isolate, context, *member.type->dictionary,
                               NewPlainObject(isolate));
    }
    return NewPlainObject(isolate);
  }
  if (literal.size() >= 2 && literal.front() == '"' && literal.back() == '"') {
    return ToV8StringFromUtf8(isolate, literal.substr(1, literal.size() - 2));
  }
  if (literal == "Infinity" || literal == "-Infinity" || literal == "NaN") {
    return v8::String::NewFromUtf8(isolate, member.default_value)
        .ToLocalChecked()
        ->ToNumber(context);
  }
  double number;
  CHECK(base::StringToDouble(literal, &number)) << literal;
  if (member.type->kind == IdlTypeKind::kBigInt) {
    return v8::BigInt::New(isolate, static_cast<int64_t>(number));
  }
  return v8::Number::New(isolate, number);
}

v8::MaybeLocal<v8::Value> DefaultDictionary(v8::Isolate* isolate,
                                            v8::Local<v8::Context> context,
                                            const DictionaryInfo& dictionary,
                                            v8::Local<v8::Object> object) {
  if (dictionary.parent &&
      DefaultDictionary(isolate, context, *dictionary.parent, object)
          .IsEmpty()) {
    return {};
  }
  const PerIsolateData& data = *PerIsolateData::From(isolate);
  for (const DictionaryMemberInfo& member : dictionary.members) {
    if (!member.default_value ||
        (member.condition && !member.condition(data))) {
      continue;
    }
    v8::Local<v8::Value> value;
    if (!DefaultFromLiteral(isolate, context, member).ToLocal(&value) ||
        object
            ->CreateDataProperty(
                context, ToV8StringFromUtf8(isolate, member.name), value)
            .IsNothing()) {
      return {};
    }
  }
  return object;
}

v8::Local<v8::Value> EmptyBuffer(v8::Isolate* isolate,
                                 const IdlTypeInfo& type) {
  const v8::Local<v8::ArrayBuffer> buffer = v8::ArrayBuffer::New(isolate, 0);
  switch (type.kind) {
    case IdlTypeKind::kArrayBuffer:
      return buffer;
    case IdlTypeKind::kSharedArrayBuffer:
      return v8::SharedArrayBuffer::New(isolate, 0);
    case IdlTypeKind::kDataView:
      return v8::DataView::New(buffer, 0, 0);
    case IdlTypeKind::kArrayBufferView:
      return v8::Uint8Array::New(buffer, 0, 0);
    case IdlTypeKind::kTypedArray:
      break;
    default:
      NOTREACHED();
  }
  const std::string_view name = type.name;
  if (name == "Int8Array") {
    return v8::Int8Array::New(buffer, 0, 0);
  }
  if (name == "Int16Array") {
    return v8::Int16Array::New(buffer, 0, 0);
  }
  if (name == "Int32Array") {
    return v8::Int32Array::New(buffer, 0, 0);
  }
  if (name == "Uint8Array") {
    return v8::Uint8Array::New(buffer, 0, 0);
  }
  if (name == "Uint16Array") {
    return v8::Uint16Array::New(buffer, 0, 0);
  }
  if (name == "Uint32Array") {
    return v8::Uint32Array::New(buffer, 0, 0);
  }
  if (name == "Uint8ClampedArray") {
    return v8::Uint8ClampedArray::New(buffer, 0, 0);
  }
  if (name == "BigInt64Array") {
    return v8::BigInt64Array::New(buffer, 0, 0);
  }
  if (name == "BigUint64Array") {
    return v8::BigUint64Array::New(buffer, 0, 0);
  }
  if (name == "Float16Array") {
    return v8::Float16Array::New(buffer, 0, 0);
  }
  if (name == "Float32Array") {
    return v8::Float32Array::New(buffer, 0, 0);
  }
  CHECK_EQ(name, "Float64Array");
  return v8::Float64Array::New(buffer, 0, 0);
}

v8::MaybeLocal<v8::Value> ResolvedPromise(v8::Local<v8::Context> context,
                                          v8::Local<v8::Value> value) {
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver) ||
      resolver->Resolve(context, value).IsNothing()) {
    return {};
  }
  return resolver->GetPromise();
}

v8::MaybeLocal<v8::Value> DefaultOf(v8::Isolate* isolate,
                                    v8::Local<v8::Context> context,
                                    const IdlTypeInfo& type) {
  if (type.flags & kIdlTypeNullable) {
    return v8::Null(isolate);
  }
  switch (type.kind) {
    case IdlTypeKind::kAny:
    case IdlTypeKind::kUndefined:
      return v8::Undefined(isolate);
    case IdlTypeKind::kBoolean:
      return v8::False(isolate);
    case IdlTypeKind::kByte:
    case IdlTypeKind::kOctet:
    case IdlTypeKind::kShort:
    case IdlTypeKind::kUnsignedShort:
    case IdlTypeKind::kLong:
    case IdlTypeKind::kUnsignedLong:
    case IdlTypeKind::kLongLong:
    case IdlTypeKind::kUnsignedLongLong:
    case IdlTypeKind::kFloat:
    case IdlTypeKind::kUnrestrictedFloat:
    case IdlTypeKind::kDouble:
    case IdlTypeKind::kUnrestrictedDouble:
      return v8::Integer::New(isolate, 0);
    case IdlTypeKind::kBigInt:
      return v8::BigInt::New(isolate, 0);
    case IdlTypeKind::kDOMString:
    case IdlTypeKind::kByteString:
    case IdlTypeKind::kUSVString:
      return v8::String::Empty(isolate);
    case IdlTypeKind::kObject:
    case IdlTypeKind::kRecord:
      return NewPlainObject(isolate);
    case IdlTypeKind::kArrayBuffer:
    case IdlTypeKind::kSharedArrayBuffer:
    case IdlTypeKind::kArrayBufferView:
    case IdlTypeKind::kDataView:
    case IdlTypeKind::kTypedArray:
      return EmptyBuffer(isolate, type);
    case IdlTypeKind::kInterface:
      return NewObjectOfInterface(context, *type.interface);
    case IdlTypeKind::kDictionary:
      return DefaultDictionary(isolate, context, *type.dictionary,
                               NewPlainObject(isolate));
    case IdlTypeKind::kEnumeration:
      return ToV8StringFromUtf8(isolate, type.values[0]);
    case IdlTypeKind::kSequence:
    case IdlTypeKind::kObservableArray:
      return v8::Array::New(isolate, 0);
    case IdlTypeKind::kFrozenArray: {
      const v8::Local<v8::Array> array = v8::Array::New(isolate, 0);
      if (array->SetIntegrityLevel(context, v8::IntegrityLevel::kFrozen)
              .IsNothing()) {
        return {};
      }
      return array;
    }
    case IdlTypeKind::kPromise: {
      v8::Local<v8::Value> value;
      if (!DefaultOf(isolate, context, *type.members[0]).ToLocal(&value)) {
        return {};
      }
      return ResolvedPromise(context, value);
    }
    case IdlTypeKind::kUnion:
      // The first member that has a default, which the generator checked one
      // has (known_gaps.py).
      for (const IdlTypeInfo* member : type.members) {
        if (HasDefault(*member)) {
          return DefaultOf(isolate, context, *member);
        }
      }
      NOTREACHED();
    case IdlTypeKind::kSymbol:
    case IdlTypeKind::kCallbackInterface:
    case IdlTypeKind::kCallbackFunction:
    case IdlTypeKind::kIterator:
      // The generator refuses a Known Gap of these (known_gaps.py).
      NOTREACHED();
  }
  NOTREACHED();
}

// What a getter gives on its receiver: what its setter stored there, or what
// it gave before, or its type's default, which it keeps there if it is an
// object, unless the IDL says [NewObject].
void GetValue(const v8::FunctionCallbackInfo<v8::Value>& info,
              const MemberInfo& member) {
  v8::Isolate* const isolate = info.GetIsolate();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::Local<v8::Private> key =
      PerIsolateData::From(isolate)->KnownGapPrivate(isolate, member.number);
  const v8::Local<v8::Object> receiver = info.This();
  bool has = false;
  if (!receiver->HasPrivate(context, key).To(&has)) {
    return;
  }
  v8::Local<v8::Value> value;
  if (has) {
    if (receiver->GetPrivate(context, key).ToLocal(&value)) {
      info.GetReturnValue().Set(value);
    }
    return;
  }
  if (!DefaultOf(isolate, context, *member.type).ToLocal(&value)) {
    return;
  }
  if (value->IsObject() && !(member.flags & kMemberNewObject) &&
      receiver->SetPrivate(context, key, value).IsNothing()) {
    return;
  }
  info.GetReturnValue().Set(value);
}

// bind_gen's make_steps_of_put_forwards(): the setter sets the attribute
// `forward` of what the getter gives.
void PutForwards(const v8::FunctionCallbackInfo<v8::Value>& info,
                 const MemberInfo& member) {
  v8::Isolate* const isolate = info.GetIsolate();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Value> target;
  if (!info.This()
           ->Get(context, ToV8StringFromUtf8(isolate, member.name))
           .ToLocal(&target)) {
    return;
  }
  if (!target->IsObject()) {
    ExceptionState exception_state(isolate);
    exception_state.ThrowTypeError(
        base::StrCat({"The attribute '", member.name, "' is not an object."}));
    return;
  }
  std::ignore = target.As<v8::Object>()->Set(
      context, ToV8StringFromUtf8(isolate, member.forward), info[0]);
}

bool IncludesNullableOrDictionary(const IdlTypeInfo& type) {
  if ((type.flags & kIdlTypeNullable) ||
      type.kind == IdlTypeKind::kDictionary) {
    return true;
  }
  if (type.kind == IdlTypeKind::kUnion) {
    for (const IdlTypeInfo* member : type.members) {
      if (IncludesNullableOrDictionary(*member)) {
        return true;
      }
    }
  }
  return false;
}

// bind_gen's make_check_argument_length() for a setter: a setter called with
// no argument throws for the types that undefined converts to without an
// error, and with [PutForwards].
bool SetterLacksItsArgument(const v8::FunctionCallbackInfo<v8::Value>& info,
                            const MemberInfo& member) {
  if (info.Length() >= 1) {
    return false;
  }
  const IdlTypeInfo& type = *member.type;
  if (!(member.flags & kMemberPutForwards) &&
      !IncludesNullableOrDictionary(type) && type.kind != IdlTypeKind::kAny &&
      !(type.flags & kIdlTypeLegacyTreatNonObjectAsNull)) {
    return false;
  }
  ExceptionState exception_state(info.GetIsolate());
  exception_state.ThrowTypeError(ExceptionMessages::NotEnoughArguments(1, 0));
  return true;
}

// What an operation or a constructor does with its arguments: the overload
// they choose, each converted, into `converted`. Returns null if that threw.
const OverloadInfo* Arguments(const v8::FunctionCallbackInfo<v8::Value>& info,
                              const OperationInfo& operation,
                              std::vector<v8::Local<v8::Value>>& converted) {
  ExceptionState exception_state(info.GetIsolate());
  return ConvertArguments(info, operation, converted, exception_state);
}

// The object behind a new object of `info`, made with a constructor's
// converted arguments: for an Event, its type and EventInit.
ScriptWrappable* MakeObjectBehind(
    v8::Isolate* isolate,
    const WrapperTypeInfo& info,
    const std::vector<v8::Local<v8::Value>>& arguments) {
  const PerIsolateData::ObjectHooks& hooks =
      PerIsolateData::From(isolate)->object_hooks();
  switch (info.table->behind) {
    case ObjectBehind::kNone:
      return nullptr;
    case ObjectBehind::kEventTarget:
      return hooks.create_event_target(info);
    case ObjectBehind::kEvent: {
      std::u16string type;
      std::array<bool, 3> flags = {false, false, false};
      if (!arguments.empty() && arguments[0]->IsString()) {
        std::ignore = FromV8String(isolate, arguments[0], &type);
      }
      if (arguments.size() > 1 && arguments[1]->IsObject()) {
        // The converted dictionary, an object of the bindings' own with a
        // null prototype, so that reading it runs no code of the page.
        const v8::Local<v8::Context> context = isolate->GetCurrentContext();
        const v8::Local<v8::Object> init = arguments[1].As<v8::Object>();
        constexpr std::array<const char*, 3> kNames = {"bubbles", "cancelable",
                                                       "composed"};
        for (size_t i = 0; i < kNames.size(); ++i) {
          v8::Local<v8::Value> value;
          if (init->Get(context, ToV8StringFromUtf8(isolate, kNames[i]))
                  .ToLocal(&value)) {
            flags[i] = value->IsTrue();
          }
        }
      }
      return hooks.create_event(info, type, flags[0], flags[1], flags[2]);
    }
  }
  NOTREACHED();
}

// A constructor's steps once its arguments have converted: the object that V8
// made for `new`, with the class's object behind it where there is one.
void Construct(const v8::FunctionCallbackInfo<v8::Value>& info,
               const WrapperTypeInfo& interface,
               const OperationInfo& operation,
               uint16_t number,
               std::string_view name) {
  v8::Isolate* const isolate = info.GetIsolate();
  std::vector<v8::Local<v8::Value>> arguments;
  if (!Arguments(info, operation, arguments)) {
    return;
  }
  Report(isolate, number, name);
  if (ScriptWrappable* const object =
          MakeObjectBehind(isolate, interface, arguments)) {
    object->AssociateWithWrapper(isolate, info.This());
  }
  info.GetReturnValue().Set(info.This());
}

}  // namespace

bool IsSafeObjectType(const WrapperTypeInfo& info) {
  // The generator gives a table to every interface whose objects a Known Gap
  // may make, and a class to the others.
  return info.table != nullptr;
}

v8::MaybeLocal<v8::Value> NewObjectOfInterface(v8::Local<v8::Context> context,
                                               const WrapperTypeInfo& info) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  CHECK(IsSafeObjectType(info));
  if (ScriptWrappable* const object = MakeObjectBehind(isolate, info, {})) {
    return object->ToV8(isolate);
  }
  std::ignore = InterfaceObject(context, info);
  return PerIsolateData::From(isolate)
      ->InterfaceTemplate(isolate, &info)
      ->InstanceTemplate()
      ->NewInstance(context);
}

void KnownGapAttributeGetter(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* const isolate = info.GetIsolate();
  const MemberInfo& member = MemberOf(info);
  if ((member.flags & kMemberLenientThis) &&
      !IsObjectOf(isolate, *member.owner, info.This())) {
    return;
  }
  if (member.flags & kMemberRefused) {
    ThrowNotImplemented(isolate);
    return;
  }
  ReportMember(isolate, member);
  GetValue(info, member);
}

void KnownGapAttributeSetter(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* const isolate = info.GetIsolate();
  const MemberInfo& member = MemberOf(info);
  if ((member.flags & kMemberLenientThis) &&
      !IsObjectOf(isolate, *member.owner, info.This())) {
    return;
  }
  if (member.flags & kMemberLenientSetter) {
    // [LegacyLenientSetter]: a setter that does nothing.
    return;
  }
  if (member.flags & kMemberReplaceable) {
    // [Replaceable]: bind_gen's make_steps_of_replaceable(), a data property of
    // the page's value on the receiver, whatever its type, after the check of
    // make_check_argument_length().
    if (info.Length() < 1) {
      ThrowTypeError(isolate, ExceptionMessages::NotEnoughArguments(1, 0));
      return;
    }
    std::ignore = info.This()->CreateDataProperty(
        isolate->GetCurrentContext(), ToV8StringFromUtf8(isolate, member.name),
        info[0]);
    return;
  }
  if (SetterLacksItsArgument(info, member)) {
    return;
  }
  if (member.flags & kMemberPutForwards) {
    PutForwards(info, member);
    return;
  }
  if (member.flags & kMemberRefused) {
    ThrowNotImplemented(isolate);
    return;
  }
  ExceptionState exception_state(isolate);
  v8::Local<v8::Value> value;
  if (member.type->kind == IdlTypeKind::kEnumeration) {
    // Web IDL: an attribute's setter ignores a string that is not one of the
    // enumeration's values, as bind_gen's setters do.
    v8::Local<v8::String> string;
    if (!info[0]->ToString(isolate->GetCurrentContext()).ToLocal(&string)) {
      return;
    }
    if (!IsEnumerationValue(isolate, *member.type, string)) {
      return;
    }
    value = string;
  } else if (!ConvertToIdl(isolate, *member.type, info[0], -1, exception_state)
                  .ToLocal(&value)) {
    return;
  }
  ReportMember(isolate, member);
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  std::ignore = info.This()->SetPrivate(
      context,
      PerIsolateData::From(isolate)->KnownGapPrivate(isolate, member.number),
      value);
}

void KnownGapOperation(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* const isolate = info.GetIsolate();
  const MemberInfo& member = MemberOf(info);
  if (member.flags & kMemberRefused) {
    ThrowNotImplemented(isolate);
    return;
  }
  std::vector<v8::Local<v8::Value>> arguments;
  const OverloadInfo* const overload =
      Arguments(info, *member.operation, arguments);
  if (!overload) {
    return;
  }
  ReportMember(isolate, member);
  v8::Local<v8::Value> value;
  if (DefaultOf(isolate, isolate->GetCurrentContext(), *overload->return_type)
          .ToLocal(&value)) {
    info.GetReturnValue().Set(value);
  }
}

void KnownGapPromiseOperation(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* const isolate = info.GetIsolate();
  const MemberInfo& member = MemberOf(info);
  // Web IDL: an error before the operation returns rejects the promise.
  ExceptionToRejectPromiseScope reject_promise_scope(info);
  if (!(member.flags & kMemberStatic) &&
      !IsObjectOf(isolate, *member.owner, info.This())) {
    ThrowTypeError(isolate, "Illegal invocation");
    return;
  }
  // A receiver of another kind, as bind_gen's make_check_receiver() checks it
  // for an operation that returns a promise: thrown in the call, so that the
  // call's context goes in front of the message, and then into the promise.
  KnownGapOperation(info);
}

void KnownGapConstructor(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* const isolate = info.GetIsolate();
  if (!info.IsConstructCall()) {
    ThrowTypeError(isolate, ExceptionMessages::ConstructorCalledAsFunction());
    return;
  }
  const auto& interface = *static_cast<const WrapperTypeInfo*>(
      info.Data().As<v8::External>()->Value(kWrapperTypeInfoTag));
  const InterfaceTable& table = *interface.table;
  if (table.flags & kInterfaceConstructorRefused) {
    ThrowNotImplemented(isolate);
    return;
  }
  Construct(info, interface, *table.constructor_operation,
            table.constructor_number,
            base::StrCat({"new ", interface.interface_name}));
}

void KnownGapFactoryFunction(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* const isolate = info.GetIsolate();
  if (!info.IsConstructCall()) {
    ThrowTypeError(isolate, ExceptionMessages::ConstructorCalledAsFunction());
    return;
  }
  const auto& construct = *static_cast<const ExposedConstructInfo*>(
      info.Data().As<v8::External>()->Value(kExposedConstructInfoTag));
  if (construct.refused) {
    ThrowNotImplemented(isolate);
    return;
  }
  Construct(info, *construct.target, *construct.operation, construct.number,
            base::StrCat({"new ", construct.name}));
}

}  // namespace mitto::bindings
