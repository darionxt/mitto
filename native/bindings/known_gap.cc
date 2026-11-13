#include "mitto/bindings/known_gap.h"

#include <stdint.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
      return type.interface && IsSafeObjectType(*type.interface);
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
    case IdlTypeKind::kIterator:
      // An iterator over nothing.
      return NewObjectOfInterface(context, *type.interface);
    case IdlTypeKind::kSymbol:
    case IdlTypeKind::kCallbackInterface:
    case IdlTypeKind::kCallbackFunction:
      // The generator refuses a Known Gap of these (known_gaps.py).
      NOTREACHED();
  }
  NOTREACHED();
}

// Web IDL's default toJSON steps: a new object with each attribute that they
// collect, read through the receiver as a page would read it, so that an
// interface that declares an attribute again, as DOMRect does x, gives its own
// getter's value, as Chrome's classes do.
void DefaultToJSON(const v8::FunctionCallbackInfo<v8::Value>& info,
                   const MemberInfo& member) {
  v8::Isolate* const isolate = info.GetIsolate();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const PerIsolateData& data = *PerIsolateData::From(isolate);
  const v8::Local<v8::Object> result = v8::Object::New(isolate);
  for (const JsonAttributeInfo& attribute :
       DefaultToJSONAttributes(member.value)) {
    if (attribute.condition && !attribute.condition(data)) {
      continue;
    }
    const v8::Local<v8::String> key = ToV8StringFromUtf8(isolate, attribute.name);
    v8::Local<v8::Value> value;
    if (!info.This()->Get(context, key).ToLocal(&value) ||
        result->CreateDataProperty(context, key, value).IsNothing()) {
      return;
    }
  }
  info.GetReturnValue().Set(result);
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
    // bind_gen's message, which names no attribute.
    ExceptionState exception_state(isolate);
    exception_state.ThrowTypeError("The attribute value is not an object");
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

// The event type of an event handler attribute: its name without "on", but for
// the WebKit-prefixed ones whose events are named in camel case, as Blink's
// DEFINE_ATTRIBUTE_EVENT_LISTENER lines in event_target.h have them.
std::u16string EventTypeOf(const MemberInfo& member) {
  const std::string_view name = std::string_view(member.name).substr(2);
  constexpr std::pair<std::string_view, std::u16string_view> kCamelCase[] = {
      {"webkitanimationend", u"webkitAnimationEnd"},
      {"webkitanimationiteration", u"webkitAnimationIteration"},
      {"webkitanimationstart", u"webkitAnimationStart"},
      {"webkittransitionend", u"webkitTransitionEnd"},
  };
  for (const auto& [attribute, type] : kCamelCase) {
    if (name == attribute) {
      return std::u16string(type);
    }
  }
  return std::u16string(name.begin(), name.end());
}

// An event handler attribute of an object with an EventTarget behind it: its
// listener there, which the target's dispatch runs (event_handlers.h in the
// DOM). Returns false for an object with none, whose attribute keeps its
// handler as any Known Gap keeps a value.
bool GetEventHandler(const v8::FunctionCallbackInfo<v8::Value>& info,
                     const MemberInfo& member) {
  v8::Isolate* const isolate = info.GetIsolate();
  const PerIsolateData::ObjectHooks& hooks =
      PerIsolateData::From(isolate)->object_hooks();
  v8::Local<v8::Value> handler;
  if (!hooks.get_event_handler ||
      !hooks.get_event_handler(isolate, info.This(), EventTypeOf(member),
                               &handler)) {
    return false;
  }
  info.GetReturnValue().Set(handler);
  return true;
}

bool SetEventHandler(const v8::FunctionCallbackInfo<v8::Value>& info,
                     const MemberInfo& member,
                     v8::Local<v8::Value> handler) {
  v8::Isolate* const isolate = info.GetIsolate();
  const PerIsolateData::ObjectHooks& hooks =
      PerIsolateData::From(isolate)->object_hooks();
  return hooks.set_event_handler &&
         hooks.set_event_handler(isolate, info.This(), EventTypeOf(member),
                                 handler);
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
// converted arguments: for an Event, its type and EventInit. For an interface
// with a class that a Known Gap may make, the object of its own class.
ScriptWrappable* MakeObjectBehind(
    v8::Isolate* isolate,
    const WrapperTypeInfo& info,
    const std::vector<v8::Local<v8::Value>>& arguments) {
  if (!info.table) {
    CHECK(info.create_for_known_gap);
    return info.create_for_known_gap(info);
  }
  const PerIsolateData::ObjectHooks& hooks =
      PerIsolateData::From(isolate)->object_hooks();
  switch (info.table->behind) {
    case ObjectBehind::kNone:
      return nullptr;
    case ObjectBehind::kClass: {
      // The nearest ancestor with a class, which the generator checked makes
      // objects for Known Gaps (known_gaps.py).
      const WrapperTypeInfo* ancestor = info.parent;
      while (ancestor->table) {
        ancestor = ancestor->parent;
      }
      CHECK(ancestor->create_for_known_gap);
      return ancestor->create_for_known_gap(info);
    }
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

// Whether a member of an event's init dictionary, of type `member`, can
// initialize the attribute of the same name, of type `attribute`: the same
// type, or a sequence for a frozen array of the same items.
bool InitializesAttribute(const IdlTypeInfo& member,
                          const IdlTypeInfo& attribute) {
  if (member.kind == IdlTypeKind::kSequence &&
      attribute.kind == IdlTypeKind::kFrozenArray) {
    return member.members.size() == 1 && attribute.members.size() == 1 &&
           InitializesAttribute(*member.members[0], *attribute.members[0]);
  }
  return &member == &attribute ||
         (member.kind == attribute.kind && member.flags == attribute.flags &&
          std::string_view(member.name) == attribute.name &&
          member.interface == attribute.interface &&
          member.dictionary == attribute.dictionary);
}

v8::MaybeLocal<v8::Value> AttributeValueOf(v8::Isolate* isolate,
                                           v8::Local<v8::Context> context,
                                           const IdlTypeInfo& type,
                                           v8::Local<v8::Value> value);

// The members of a converted dictionary, given or by their defaults, into
// `object`, in the order of the IDL, as Blink's ToV8 of a dictionary writes
// them.
bool CopyDictionary(v8::Isolate* isolate,
                    v8::Local<v8::Context> context,
                    const DictionaryInfo& dictionary,
                    v8::Local<v8::Object> converted,
                    v8::Local<v8::Object> object) {
  if (dictionary.parent && !CopyDictionary(isolate, context, *dictionary.parent,
                                           converted, object)) {
    return false;
  }
  const PerIsolateData& data = *PerIsolateData::From(isolate);
  for (const DictionaryMemberInfo& member : dictionary.members) {
    if (member.condition && !member.condition(data)) {
      continue;
    }
    const v8::Local<v8::String> key = ToV8StringFromUtf8(isolate, member.name);
    bool given = false;
    if (!converted->HasOwnProperty(context, key).To(&given)) {
      return false;
    }
    v8::Local<v8::Value> value;
    if (given) {
      v8::Local<v8::Value> item;
      if (!converted->Get(context, key).ToLocal(&item) ||
          !AttributeValueOf(isolate, context, *member.type, item)
               .ToLocal(&value)) {
        return false;
      }
    } else if (!member.default_value) {
      continue;
    } else if (!DefaultFromLiteral(isolate, context, member).ToLocal(&value)) {
      return false;
    }
    if (object->CreateDataProperty(context, key, value).IsNothing()) {
      return false;
    }
  }
  return true;
}

// What an attribute gives once a converted value of its type initializes it,
// as Blink's ToV8 gives the member that its event class keeps: a sequence
// becomes a frozen array, and a dictionary an object of the page's.
v8::MaybeLocal<v8::Value> AttributeValueOf(v8::Isolate* isolate,
                                           v8::Local<v8::Context> context,
                                           const IdlTypeInfo& type,
                                           v8::Local<v8::Value> value) {
  switch (type.kind) {
    case IdlTypeKind::kDictionary: {
      if (!value->IsObject()) {
        return value;
      }
      const v8::Local<v8::Object> object = NewPlainObject(isolate);
      if (!CopyDictionary(isolate, context, *type.dictionary,
                          value.As<v8::Object>(), object)) {
        return {};
      }
      return object;
    }
    case IdlTypeKind::kSequence:
    case IdlTypeKind::kFrozenArray: {
      if (!value->IsArray()) {
        return value;
      }
      const v8::Local<v8::Array> items = value.As<v8::Array>();
      std::vector<v8::Local<v8::Value>> copied;
      copied.reserve(items->Length());
      for (uint32_t i = 0; i < items->Length(); ++i) {
        v8::Local<v8::Value> item;
        v8::Local<v8::Value> copy;
        if (!items->Get(context, i).ToLocal(&item) ||
            !AttributeValueOf(isolate, context, *type.members[0], item)
                 .ToLocal(&copy)) {
          return {};
        }
        copied.push_back(copy);
      }
      const v8::Local<v8::Array> array =
          v8::Array::New(isolate, copied.data(), copied.size());
      if (type.kind == IdlTypeKind::kFrozenArray &&
          array->SetIntegrityLevel(context, v8::IntegrityLevel::kFrozen)
              .IsNothing()) {
        return {};
      }
      return array;
    }
    default:
      return value;
  }
}

// The attribute named `name` of an object of `interface` that a Known Gap
// gives, on it or on an ancestor without a class, or null.
const MemberInfo* KnownGapAttribute(const PerIsolateData& data,
                                    const WrapperTypeInfo& interface,
                                    std::string_view name) {
  constexpr uint16_t kNotGiven =
      kMemberStatic | kMemberEventHandler | kMemberRefused;
  for (const WrapperTypeInfo* type = &interface; type && type->table;
       type = type->parent) {
    for (const base::span<const MemberInfo> members :
         {type->table->template_members, type->table->context_members}) {
      for (const MemberInfo& member : members) {
        if (member.kind == MemberKind::kAttribute &&
            member.target != MemberTarget::kInterface &&
            !(member.flags & kNotGiven) &&
            std::string_view(member.name) == name &&
            (!member.condition || member.condition(data))) {
          return &member;
        }
      }
    }
  }
  return nullptr;
}

// The DOM Standard's inner event creation steps: each member of an event's
// init dictionary, given or by its default, initializes the attribute of the
// same name, as each of Blink's event classes does in its constructor. A
// Known Gap attribute gives it from then on, as what a setter stored. Returns
// false if that threw.
bool InitializeEventAttributes(v8::Isolate* isolate,
                               v8::Local<v8::Object> event,
                               const WrapperTypeInfo& interface,
                               const DictionaryInfo& dictionary,
                               v8::Local<v8::Object> init) {
  if (dictionary.parent &&
      !InitializeEventAttributes(isolate, event, interface, *dictionary.parent,
                                 init)) {
    return false;
  }
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  PerIsolateData& data = *PerIsolateData::From(isolate);
  for (const DictionaryMemberInfo& member : dictionary.members) {
    if (member.condition && !member.condition(data)) {
      continue;
    }
    const MemberInfo* const attribute =
        KnownGapAttribute(data, interface, member.name);
    if (!attribute || !InitializesAttribute(*member.type, *attribute->type)) {
      continue;
    }
    const v8::Local<v8::String> key = ToV8StringFromUtf8(isolate, member.name);
    bool given = false;
    if (!init->HasOwnProperty(context, key).To(&given)) {
      return false;
    }
    v8::Local<v8::Value> item;
    if (given) {
      if (!init->Get(context, key).ToLocal(&item)) {
        return false;
      }
    } else if (!member.default_value) {
      continue;
    } else if (!DefaultFromLiteral(isolate, context, member).ToLocal(&item)) {
      return false;
    }
    v8::Local<v8::Value> value;
    if (!AttributeValueOf(isolate, context, *attribute->type, item)
             .ToLocal(&value) ||
        event
            ->SetPrivate(context,
                         data.KnownGapPrivate(isolate, attribute->number),
                         value)
            .IsNothing()) {
      return false;
    }
  }
  return true;
}

// A constructor's steps once its arguments have converted: the object that V8
// made for `new`, with the class's object behind it where there is one, and
// for an event the attributes that its init dictionary gives.
void Construct(const v8::FunctionCallbackInfo<v8::Value>& info,
               const WrapperTypeInfo& interface,
               const OperationInfo& operation,
               uint16_t number,
               std::string_view name) {
  v8::Isolate* const isolate = info.GetIsolate();
  std::vector<v8::Local<v8::Value>> arguments;
  const OverloadInfo* const overload = Arguments(info, operation, arguments);
  if (!overload) {
    return;
  }
  Report(isolate, number, name);
  if (ScriptWrappable* const object =
          MakeObjectBehind(isolate, interface, arguments)) {
    object->AssociateWithWrapper(isolate, info.This());
  }
  if (interface.table && interface.table->behind == ObjectBehind::kEvent &&
      overload->arguments.size() > 1 &&
      overload->arguments[1].type->kind == IdlTypeKind::kDictionary) {
    const v8::Local<v8::Object> init =
        arguments.size() > 1 && arguments[1]->IsObject()
            ? arguments[1].As<v8::Object>()
            : v8::Object::New(isolate, v8::Null(isolate), nullptr, nullptr, 0);
    if (!InitializeEventAttributes(isolate, info.This(), interface,
                                   *overload->arguments[1].type->dictionary,
                                   init)) {
      return;
    }
  }
  info.GetReturnValue().Set(info.This());
}

// The items of an iterator over a list, and the index of the next one, which
// NewIteratorOverList() keeps on the iterator.
v8::Local<v8::Private> IteratorItemsKey(v8::Isolate* isolate) {
  return v8::Private::ForApi(
      isolate, v8::String::NewFromUtf8Literal(isolate, "mitto:iterator items"));
}

v8::Local<v8::Private> IteratorIndexKey(v8::Isolate* isolate) {
  return v8::Private::ForApi(
      isolate, v8::String::NewFromUtf8Literal(isolate, "mitto:iterator index"));
}

// The next item of an iterator over a list into `value`, or false when it has
// none left or is over nothing. Returns nothing if that threw.
v8::Maybe<bool> NextItemOfList(v8::Local<v8::Context> context,
                               v8::Local<v8::Object> iterator,
                               v8::Local<v8::Value>* value) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::Value> items;
  v8::Local<v8::Value> index;
  if (!iterator->GetPrivate(context, IteratorItemsKey(isolate))
           .ToLocal(&items) ||
      !iterator->GetPrivate(context, IteratorIndexKey(isolate))
           .ToLocal(&index)) {
    return v8::Nothing<bool>();
  }
  if (!items->IsArray()) {
    return v8::Just(false);
  }
  const uint32_t next = index.As<v8::Uint32>()->Value();
  const v8::Local<v8::Array> array = items.As<v8::Array>();
  if (next >= array->Length()) {
    return v8::Just(false);
  }
  if (!array->Get(context, next).ToLocal(value) ||
      iterator
          ->SetPrivate(context, IteratorIndexKey(isolate),
                       v8::Integer::NewFromUnsigned(isolate, next + 1))
          .IsNothing()) {
    return v8::Nothing<bool>();
  }
  return v8::Just(true);
}

}  // namespace

bool InitializeEventOfInterface(v8::Isolate* isolate,
                                v8::Local<v8::Object> event,
                                const WrapperTypeInfo& interface,
                                v8::Local<v8::Object> init) {
  const InterfaceTable* const table = interface.table;
  if (!table || !table->constructor_operation) {
    return true;
  }
  for (const OverloadInfo& overload : table->constructor_operation->overloads) {
    if (overload.arguments.size() > 1 &&
        overload.arguments[1].type->kind == IdlTypeKind::kDictionary) {
      return InitializeEventAttributes(isolate, event, interface,
                                       *overload.arguments[1].type->dictionary,
                                       init);
    }
  }
  return true;
}

bool SetEventOfInterfaceAttribute(v8::Isolate* isolate,
                                  ScriptWrappable& event,
                                  std::string_view name,
                                  v8::Local<v8::Value> value) {
  PerIsolateData& data = *PerIsolateData::From(isolate);
  const MemberInfo* const attribute =
      KnownGapAttribute(data, *event.GetWrapperTypeInfo(), name);
  if (!attribute) {
    return false;
  }
  return event.ToV8(isolate)
      ->SetPrivate(isolate->GetCurrentContext(),
                   data.KnownGapPrivate(isolate, attribute->number), value)
      .FromMaybe(false);
}

v8::Local<v8::Value> EventOfInterfaceAttribute(v8::Isolate* isolate,
                                               ScriptWrappable& event,
                                               std::string_view name) {
  PerIsolateData& data = *PerIsolateData::From(isolate);
  const MemberInfo* const attribute =
      KnownGapAttribute(data, *event.GetWrapperTypeInfo(), name);
  if (!attribute) {
    return v8::Undefined(isolate);
  }
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::Local<v8::Object> wrapper = event.ToV8(isolate);
  const v8::Local<v8::Private> key =
      data.KnownGapPrivate(isolate, attribute->number);
  v8::Local<v8::Value> value;
  if (wrapper->HasPrivate(context, key).FromMaybe(false) &&
      wrapper->GetPrivate(context, key).ToLocal(&value)) {
    return value;
  }
  if (DefaultOf(isolate, context, *attribute->type).ToLocal(&value)) {
    return value;
  }
  return v8::Undefined(isolate);
}

v8::MaybeLocal<v8::Value> NewIteratorOverList(
    v8::Local<v8::Context> context,
    const WrapperTypeInfo& iterator,
    const std::vector<v8::Local<v8::Value>>& items) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::Value> object;
  if (!NewObjectOfInterface(context, iterator).ToLocal(&object)) {
    return {};
  }
  const v8::Local<v8::Array> array = v8::Array::New(
      isolate, const_cast<v8::Local<v8::Value>*>(items.data()), items.size());
  if (object.As<v8::Object>()
          ->SetPrivate(context, IteratorItemsKey(isolate), array)
          .IsNothing() ||
      object.As<v8::Object>()
          ->SetPrivate(context, IteratorIndexKey(isolate),
                       v8::Integer::New(isolate, 0))
          .IsNothing()) {
    return {};
  }
  return object;
}

void IteratorStep(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* const isolate = info.GetIsolate();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const MemberInfo& member = MemberOf(info);
  const bool asynchronous = member.flags & kMemberReturnsPromise;
  std::optional<ExceptionToRejectPromiseScope> reject_promise_scope;
  if (asynchronous) {
    reject_promise_scope.emplace(info);
    // Without V8's signature check, as bind_gen's make_check_receiver() does
    // for an operation that returns a promise.
    if (!IsObjectOf(isolate, *member.owner, info.This())) {
      ThrowTypeError(isolate, "Illegal invocation");
      return;
    }
  }
  v8::Local<v8::Value> value = (member.flags & kMemberIteratorReturn)
                                   ? info[0]
                                   : v8::Undefined(isolate).As<v8::Value>();
  bool has_item = false;
  if (!asynchronous && !(member.flags & kMemberIteratorReturn) &&
      !NextItemOfList(context, info.This(), &value).To(&has_item)) {
    return;
  }
  // Web IDL's CreateIterResultObject(): value, then done.
  const v8::Local<v8::Object> result = v8::Object::New(isolate);
  if (result
          ->CreateDataProperty(
              context, v8::String::NewFromUtf8Literal(isolate, "value"), value)
          .IsNothing() ||
      result
          ->CreateDataProperty(context,
                               v8::String::NewFromUtf8Literal(isolate, "done"),
                               v8::Boolean::New(isolate, !has_item))
          .IsNothing()) {
    return;
  }
  if (!asynchronous) {
    info.GetReturnValue().Set(result);
    return;
  }
  v8::Local<v8::Value> promise;
  if (ResolvedPromise(context, result).ToLocal(&promise)) {
    info.GetReturnValue().Set(promise);
  }
}

bool IsSafeObjectType(const WrapperTypeInfo& info) {
  // The generator gives a table to every interface without a class, and a
  // function that makes an object to each class whose objects a Known Gap may
  // make.
  return info.table != nullptr || info.create_for_known_gap != nullptr;
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
  // An attribute whose type is a promise has no V8 signature check: bind_gen's
  // make_check_receiver() throws in the call, and the promise return context
  // rejects with that, so that "get closed" of a reader rejects on another
  // kind of object, as in Chrome.
  std::optional<ExceptionToRejectPromiseScope> reject_promise_scope;
  if ((member.flags & kMemberReturnsPromise) &&
      !(member.flags & kMemberStatic)) {
    reject_promise_scope.emplace(info);
    if (!IsObjectOf(isolate, *member.owner, info.This())) {
      ThrowTypeError(isolate, "Illegal invocation");
      return;
    }
  }
  if (member.flags & kMemberRefused) {
    ThrowNotImplemented(isolate);
    return;
  }
  ReportMember(isolate, member);
  if ((member.flags & kMemberEventHandler) && GetEventHandler(info, member)) {
    return;
  }
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
  if ((member.flags & kMemberEventHandler) &&
      SetEventHandler(info, member, value)) {
    return;
  }
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  std::ignore = info.This()->SetPrivate(
      context,
      PerIsolateData::From(isolate)->KnownGapPrivate(isolate, member.number),
      value);
}

namespace {

// An operation's steps after its receiver: its arguments, the report and its
// return type's default. `chosen` is the overload that bind_gen's overload
// dispatcher chose before the receiver was checked, or null for none yet.
void OperationSteps(const v8::FunctionCallbackInfo<v8::Value>& info,
                    const MemberInfo& member,
                    const OverloadInfo* chosen) {
  v8::Isolate* const isolate = info.GetIsolate();
  if (member.flags & kMemberRefused) {
    ThrowNotImplemented(isolate);
    return;
  }
  if (member.flags & kMemberStringifierAttribute) {
    // Blink's toString() of a stringifier attribute is the attribute's getter.
    ReportMember(isolate, member);
    GetValue(info, member);
    return;
  }
  std::vector<v8::Local<v8::Value>> arguments;
  const OverloadInfo* overload = chosen;
  if (!overload) {
    overload = Arguments(info, *member.operation, arguments);
  } else {
    ExceptionState exception_state(isolate);
    if (!ConvertOverloadArguments(info, *overload, arguments,
                                  exception_state)) {
      overload = nullptr;
    }
  }
  if (!overload) {
    return;
  }
  ReportMember(isolate, member);
  if (member.flags & kMemberDefaultToJSON) {
    DefaultToJSON(info, member);
    return;
  }
  v8::Local<v8::Value> value;
  if (DefaultOf(isolate, isolate->GetCurrentContext(), *overload->return_type)
          .ToLocal(&value)) {
    info.GetReturnValue().Set(value);
  }
}

}  // namespace

void KnownGapOperation(const v8::FunctionCallbackInfo<v8::Value>& info) {
  OperationSteps(info, MemberOf(info), nullptr);
}

void KnownGapPromiseOperation(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* const isolate = info.GetIsolate();
  const MemberInfo& member = MemberOf(info);
  // Web IDL: an error before the operation returns rejects the promise.
  ExceptionToRejectPromiseScope reject_promise_scope(info);
  // An operation of several overloads goes through bind_gen's overload
  // dispatcher first, which checks the number of arguments and tests the
  // distinguishing argument, and only then calls the overload's function,
  // which checks the receiver: so that createImageBitmap() on another kind of
  // object rejects with "1 argument required, but only 0 present.", as in
  // Chrome.
  const OverloadInfo* chosen = nullptr;
  if (!(member.flags & kMemberRefused) &&
      !(member.flags & kMemberStringifierAttribute) &&
      HasOverloads(*member.operation)) {
    ExceptionState exception_state(isolate);
    chosen = ChooseOverload(info, *member.operation, exception_state);
    if (!chosen) {
      return;
    }
  }
  // A receiver of another kind, as bind_gen's make_check_receiver() checks it
  // for an operation that returns a promise: thrown in the call, so that the
  // call's context goes in front of the message, and then into the promise.
  if (!(member.flags & kMemberStatic) &&
      !IsObjectOf(isolate, *member.owner, info.This())) {
    ThrowTypeError(isolate, "Illegal invocation");
    return;
  }
  OperationSteps(info, member, chosen);
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

void KnownGapConstructorOfClass(const v8::FunctionCallbackInfo<v8::Value>& info,
                                const WrapperTypeInfo& interface,
                                const OperationInfo& operation,
                                uint16_t number) {
  v8::Isolate* const isolate = info.GetIsolate();
  if (!info.IsConstructCall()) {
    ThrowTypeError(isolate, ExceptionMessages::ConstructorCalledAsFunction());
    return;
  }
  Construct(info, interface, operation, number,
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
