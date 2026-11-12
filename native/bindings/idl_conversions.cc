#include "mitto/bindings/idl_conversions.h"

#include <math.h>
#include <stdint.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/compiler_specific.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/notreached.h"
#include "base/numerics/clamped_math.h"
#include "base/strings/strcat.h"
#include "mitto/bindings/exception_context.h"
#include "mitto/bindings/exception_messages.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/v8_conversions.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-typed-array.h"

namespace mitto::bindings {

namespace {

// The largest integer that a double holds exactly, 2^53 - 1, which bounds
// `long long` and `unsigned long long`.
constexpr double kJSMaxInteger = 9007199254740991.0;

struct IntegerRange {
  double minimum;
  double maximum;
  // How many values the type has, for the modulo of a plain conversion, or 0
  // for the 64-bit types, which take the integer part as it is.
  double values;
};

IntegerRange RangeOf(IdlTypeKind kind) {
  switch (kind) {
    case IdlTypeKind::kByte:
      return {-128, 127, 256};
    case IdlTypeKind::kOctet:
      return {0, 255, 256};
    case IdlTypeKind::kShort:
      return {-32768, 32767, 65536};
    case IdlTypeKind::kUnsignedShort:
      return {0, 65535, 65536};
    case IdlTypeKind::kLong:
      return {-2147483648.0, 2147483647.0, 4294967296.0};
    case IdlTypeKind::kUnsignedLong:
      return {0, 4294967295.0, 4294967296.0};
    case IdlTypeKind::kLongLong:
      return {-kJSMaxInteger, kJSMaxInteger, 0};
    case IdlTypeKind::kUnsignedLongLong:
      return {0, kJSMaxInteger, 0};
    default:
      NOTREACHED();
  }
}

// Web IDL's ConvertToInt() as Blink's ToSmallerInt(), ToInt32Slow() and their
// kin in v8_binding_for_core.cc do it: ToNumber(), then [EnforceRange]'s
// checks, [Clamp]'s rounding, or the integer part modulo the type's values.
v8::MaybeLocal<v8::Value> ConvertInteger(v8::Isolate* isolate,
                                         const IdlTypeInfo& type,
                                         v8::Local<v8::Value> value,
                                         ExceptionState& exception_state) {
  double number;
  if (!value->NumberValue(isolate->GetCurrentContext()).To(&number)) {
    return {};
  }
  const IntegerRange range = RangeOf(type.kind);
  if (type.flags & kIdlTypeEnforceRange) {
    if (!std::isfinite(number)) {
      exception_state.ThrowTypeError(
          base::StrCat({"Value is", std::isinf(number) ? " infinite and" : "",
                        " not of type '", type.name, "'."}));
      return {};
    }
    number = std::trunc(number);
    if (number < range.minimum || number > range.maximum) {
      exception_state.ThrowTypeError(base::StrCat(
          {"Value is outside the '", type.name, "' value range."}));
      return {};
    }
    return v8::Number::New(isolate, number + 0.0);
  }
  if (std::isnan(number)) {
    return v8::Integer::New(isolate, 0);
  }
  if (type.flags & kIdlTypeClamp) {
    // Blink's ClampTo() truncates within the type's range, and for the 64-bit
    // types first rounds half to even, as std::nearbyint() does.
    if (range.values == 0) {
      number = std::nearbyint(number);
    }
    number = std::trunc(std::clamp(number, range.minimum, range.maximum));
    return v8::Number::New(isolate, number + 0.0);
  }
  if (std::isinf(number)) {
    return v8::Integer::New(isolate, 0);
  }
  number = std::trunc(number);
  if (range.values == 0) {
    return v8::Number::New(isolate, number + 0.0);
  }
  number = std::fmod(number, range.values);
  if (number < range.minimum) {
    number += range.values;
  } else if (number > range.maximum) {
    number -= range.values;
  }
  return v8::Number::New(isolate, number + 0.0);
}

v8::MaybeLocal<v8::Value> ConvertFloatingPoint(
    v8::Isolate* isolate,
    const IdlTypeInfo& type,
    v8::Local<v8::Value> value,
    ExceptionState& exception_state) {
  double number;
  if (!value->NumberValue(isolate->GetCurrentContext()).To(&number)) {
    return {};
  }
  const bool is_float = type.kind == IdlTypeKind::kFloat ||
                        type.kind == IdlTypeKind::kUnrestrictedFloat;
  if (is_float) {
    number = static_cast<float>(number);
  }
  const bool restricted =
      type.kind == IdlTypeKind::kFloat || type.kind == IdlTypeKind::kDouble;
  if (restricted && !std::isfinite(number)) {
    exception_state.ThrowTypeError(
        base::StrCat({"The provided ", is_float ? "float" : "double",
                      " value is non-finite."}));
    return {};
  }
  return v8::Number::New(isolate, number);
}

v8::MaybeLocal<v8::Value> ConvertString(v8::Isolate* isolate,
                                        const IdlTypeInfo& type,
                                        v8::Local<v8::Value> value,
                                        ExceptionState& exception_state) {
  if ((type.flags & kIdlTypeLegacyNullToEmptyString) && value->IsNull()) {
    return v8::String::Empty(isolate);
  }
  if (type.kind == IdlTypeKind::kByteString) {
    std::string bytes;
    if (!FromV8ByteString(isolate, value, &bytes)) {
      return {};
    }
    return ToV8ByteString(isolate, bytes);
  }
  std::u16string text;
  if (type.kind == IdlTypeKind::kUSVString
          ? !FromV8USVString(isolate, value, &text)
          : !FromV8String(isolate, value, &text)) {
    return {};
  }
  return ToV8String(isolate, text);
}

// The message of a value that is not of `name`: Blink's ArgumentNotOfType() for
// an argument, and FailedToConvertJSValue() for any other value.
std::string NotOfType(int argument_index, std::string_view name) {
  return argument_index >= 0
             ? ExceptionMessages::ArgumentNotOfType(argument_index, name)
             : ExceptionMessages::FailedToConvertJSValue(name);
}

bool IsNumeric(IdlTypeKind kind) {
  return kind >= IdlTypeKind::kByte && kind <= IdlTypeKind::kUnrestrictedDouble;
}

bool IsString(IdlTypeKind kind) {
  return kind == IdlTypeKind::kDOMString || kind == IdlTypeKind::kByteString ||
         kind == IdlTypeKind::kUSVString;
}

// The members of a union, with the members of the unions inside it, and
// whether any of them is nullable.
void Flatten(const IdlTypeInfo& type,
             std::vector<const IdlTypeInfo*>& members,
             bool& nullable) {
  for (const IdlTypeInfo* member : type.members) {
    nullable = nullable || (member->flags & kIdlTypeNullable);
    if (member->kind == IdlTypeKind::kUnion) {
      Flatten(*member, members, nullable);
    } else {
      members.push_back(member);
    }
  }
}

int Depth(const WrapperTypeInfo* info) {
  int depth = 0;
  for (; info; info = info->parent) {
    ++depth;
  }
  return depth;
}

// Whether `value` is a typed array of the kind `name`, such as Uint8Array.
bool IsTypedArrayOf(v8::Local<v8::Value> value, std::string_view name) {
  if (name == "Int8Array") {
    return value->IsInt8Array();
  }
  if (name == "Int16Array") {
    return value->IsInt16Array();
  }
  if (name == "Int32Array") {
    return value->IsInt32Array();
  }
  if (name == "BigInt64Array") {
    return value->IsBigInt64Array();
  }
  if (name == "Uint8Array") {
    return value->IsUint8Array();
  }
  if (name == "Uint16Array") {
    return value->IsUint16Array();
  }
  if (name == "Uint32Array") {
    return value->IsUint32Array();
  }
  if (name == "BigUint64Array") {
    return value->IsBigUint64Array();
  }
  if (name == "Uint8ClampedArray") {
    return value->IsUint8ClampedArray();
  }
  if (name == "Float16Array") {
    return value->IsFloat16Array();
  }
  if (name == "Float32Array") {
    return value->IsFloat32Array();
  }
  return name == "Float64Array" && value->IsFloat64Array();
}

bool IsResizable(v8::Local<v8::ArrayBuffer> buffer) {
  return buffer->IsResizableByUserJavaScript();
}

// A buffer source, as NativeValueTraits of DOMArrayBuffer, its views and the
// spans that [PassAsSpan] gives check it.
v8::MaybeLocal<v8::Value> ConvertBuffer(const IdlTypeInfo& type,
                                        v8::Local<v8::Value> value,
                                        int argument_index,
                                        ExceptionState& exception_state) {
  const bool allow_shared = type.flags & kIdlTypeAllowShared;
  const bool allow_resizable = type.flags & kIdlTypeAllowResizable;
  bool matches = false;
  switch (type.kind) {
    case IdlTypeKind::kArrayBuffer:
      matches = value->IsArrayBuffer() ||
                (allow_shared && value->IsSharedArrayBuffer());
      break;
    case IdlTypeKind::kSharedArrayBuffer:
      matches = value->IsSharedArrayBuffer();
      break;
    case IdlTypeKind::kArrayBufferView:
      matches = value->IsArrayBufferView();
      break;
    case IdlTypeKind::kDataView:
      matches = value->IsDataView();
      break;
    case IdlTypeKind::kTypedArray:
      matches = IsTypedArrayOf(value, type.name);
      break;
    default:
      NOTREACHED();
  }
  if (!matches) {
    exception_state.ThrowTypeError(NotOfType(argument_index, type.name));
    return {};
  }
  if (value->IsArrayBuffer() && !allow_resizable &&
      IsResizable(value.As<v8::ArrayBuffer>())) {
    exception_state.ThrowTypeError(
        "The provided ArrayBuffer value must not be resizable");
    return {};
  }
  if (value->IsSharedArrayBuffer() && !allow_resizable &&
      value.As<v8::SharedArrayBuffer>()
          ->GetBackingStore()
          ->IsResizableByUserJavaScript()) {
    exception_state.ThrowTypeError(
        "The provided SharedArrayBuffer value must not be resizable");
    return {};
  }
  if (value->IsArrayBufferView()) {
    const v8::Local<v8::ArrayBufferView> view = value.As<v8::ArrayBufferView>();
    if (view->HasBuffer()) {
      const v8::Local<v8::ArrayBuffer> buffer = view->Buffer();
      if (!allow_resizable && IsResizable(buffer)) {
        exception_state.ThrowTypeError(
            "The provided ArrayBuffer value must not be resizable");
        return {};
      }
      if (!allow_shared && buffer->GetBackingStore()->IsShared()) {
        exception_state.ThrowTypeError(
            "The provided ArrayBufferView value must not be shared.");
        return {};
      }
    }
  }
  return value;
}

v8::MaybeLocal<v8::Value> Convert(v8::Isolate* isolate,
                                  const IdlTypeInfo& type,
                                  v8::Local<v8::Value> value,
                                  int argument_index,
                                  ExceptionState& exception_state);

// A dictionary, as bind_gen's dictionary.py fills one: the inherited members
// first, each read once, in order, with the context of the member being read
// in front of an error. Its value is an object of the bindings' own, with a
// null prototype, of the members that were given, converted.
bool FillDictionary(v8::Isolate* isolate,
                    const DictionaryInfo& dictionary,
                    v8::Local<v8::Value> value,
                    v8::Local<v8::Object> result,
                    ExceptionState& exception_state) {
  if (dictionary.parent && !FillDictionary(isolate, *dictionary.parent, value,
                                           result, exception_state)) {
    return false;
  }
  if (dictionary.members.empty()) {
    return true;
  }
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const PerIsolateData& data = *PerIsolateData::From(isolate);
  DictionaryConversionScope scope(isolate, dictionary.name);
  for (const DictionaryMemberInfo& member : dictionary.members) {
    if (member.condition && !member.condition(data)) {
      continue;
    }
    scope.SetCurrentPropertyName(member.name);
    const v8::Local<v8::String> key = ToV8StringFromUtf8(isolate, member.name);
    v8::Local<v8::Value> item = v8::Undefined(isolate);
    if (!value->IsNullOrUndefined() &&
        !value.As<v8::Object>()->Get(context, key).ToLocal(&item)) {
      return false;
    }
    if (item->IsUndefined()) {
      if (member.required) {
        exception_state.ThrowTypeError("Required member is undefined.");
        return false;
      }
      continue;
    }
    v8::Local<v8::Value> converted;
    if (!Convert(isolate, *member.type, item, -1, exception_state)
             .ToLocal(&converted) ||
        result->CreateDataProperty(context, key, converted).IsNothing()) {
      return false;
    }
  }
  return true;
}

v8::MaybeLocal<v8::Value> ConvertDictionary(v8::Isolate* isolate,
                                            const IdlTypeInfo& type,
                                            v8::Local<v8::Value> value,
                                            ExceptionState& exception_state) {
  if (!value->IsNullOrUndefined() && !value->IsObject()) {
    exception_state.ThrowTypeError(
        ExceptionMessages::ValueNotOfType(type.dictionary->name));
    return {};
  }
  const v8::Local<v8::Object> result =
      v8::Object::New(isolate, v8::Null(isolate), nullptr, nullptr, 0);
  if (!FillDictionary(isolate, *type.dictionary, value, result,
                      exception_state)) {
    return {};
  }
  return result;
}

v8::MaybeLocal<v8::Value> ConvertSequence(v8::Isolate* isolate,
                                          const IdlTypeInfo& type,
                                          v8::Local<v8::Value> value,
                                          v8::Local<v8::Function> method,
                                          ExceptionState& exception_state) {
  std::vector<v8::Local<v8::Value>> items;
  const auto each = [&](v8::Local<v8::Value> item) {
    v8::Local<v8::Value> converted;
    if (!Convert(isolate, *type.members[0], item, -1, exception_state)
             .ToLocal(&converted)) {
      return false;
    }
    items.push_back(converted);
    return true;
  };
  const bool done = method.IsEmpty()
                        ? IterateSequence(isolate, value, exception_state, each)
                        : IterateSequenceWithMethod(
                              isolate, value.As<v8::Object>(), method, each);
  if (!done) {
    return {};
  }
  const v8::Local<v8::Array> array =
      v8::Array::New(isolate, items.data(), items.size());
  if (type.kind == IdlTypeKind::kFrozenArray &&
      array
          ->SetIntegrityLevel(isolate->GetCurrentContext(),
                              v8::IntegrityLevel::kFrozen)
          .IsNothing()) {
    return {};
  }
  return array;
}

v8::MaybeLocal<v8::Value> ConvertRecord(v8::Isolate* isolate,
                                        const IdlTypeInfo& type,
                                        v8::Local<v8::Value> value,
                                        ExceptionState& exception_state) {
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::Local<v8::Object> result = v8::Object::New(isolate);
  if (!IterateRecord(
          isolate, value, exception_state, type.name,
          [&](v8::Local<v8::Value> key, v8::Local<v8::Value> item) {
            v8::Local<v8::Value> converted_key;
            v8::Local<v8::Value> converted;
            return Convert(isolate, *type.members[0], key, -1, exception_state)
                       .ToLocal(&converted_key) &&
                   Convert(isolate, *type.members[1], item, -1, exception_state)
                       .ToLocal(&converted) &&
                   result
                       ->CreateDataProperty(
                           context, converted_key.As<v8::Name>(), converted)
                       .IsJust();
          })) {
    return {};
  }
  return result;
}

std::string Utf8Of(v8::Isolate* isolate, v8::Local<v8::String> string) {
  const v8::String::Utf8Value utf8(isolate, string);
  // SAFETY: Utf8Value holds length() bytes at *utf8.
  return std::string(UNSAFE_BUFFERS(std::string_view(*utf8, utf8.length())));
}

v8::MaybeLocal<v8::Value> ConvertEnumeration(v8::Isolate* isolate,
                                             const IdlTypeInfo& type,
                                             v8::Local<v8::Value> value,
                                             ExceptionState& exception_state) {
  v8::Local<v8::String> string;
  if (!value->ToString(isolate->GetCurrentContext()).ToLocal(&string)) {
    return {};
  }
  if (IsEnumerationValue(isolate, type, string)) {
    return string;
  }
  const std::string text = Utf8Of(isolate, string);
  exception_state.ThrowTypeError(
      base::StrCat({"The provided value '", text,
                    "' is not a valid enum value of type ", type.name, "."}));
  return {};
}

// Web IDL's conversion to a union, in the order of bind_gen's union.py.
v8::MaybeLocal<v8::Value> ConvertUnion(v8::Isolate* isolate,
                                       const IdlTypeInfo& type,
                                       v8::Local<v8::Value> value,
                                       ExceptionState& exception_state) {
  std::vector<const IdlTypeInfo*> members;
  bool nullable = false;
  Flatten(type, members, nullable);
  const auto find = [&](auto test) -> const IdlTypeInfo* {
    for (const IdlTypeInfo* member : members) {
      if (test(*member)) {
        return member;
      }
    }
    return nullptr;
  };
  const auto convert = [&](const IdlTypeInfo* member) {
    return Convert(isolate, *member, value, -1, exception_state);
  };
  if (value->IsUndefined()) {
    if (find([](const IdlTypeInfo& t) {
          return t.kind == IdlTypeKind::kUndefined;
        })) {
      return v8::Undefined(isolate);
    }
  }
  if (nullable && value->IsNullOrUndefined()) {
    return v8::Null(isolate);
  }
  if (const IdlTypeInfo* const dictionary = find([](const IdlTypeInfo& t) {
        return t.kind == IdlTypeKind::kDictionary;
      });
      dictionary && value->IsNullOrUndefined()) {
    return convert(dictionary);
  }
  // Platform objects, from the most derived interface to the least.
  std::vector<const IdlTypeInfo*> interfaces;
  for (const IdlTypeInfo* member : members) {
    if (member->kind == IdlTypeKind::kInterface && member->interface) {
      interfaces.push_back(member);
    }
  }
  std::sort(interfaces.begin(), interfaces.end(),
            [](const IdlTypeInfo* a, const IdlTypeInfo* b) {
              const int depth_a = Depth(a->interface);
              const int depth_b = Depth(b->interface);
              if (depth_a != depth_b) {
                return depth_a > depth_b;
              }
              return std::string_view(a->interface->interface_name) >
                     std::string_view(b->interface->interface_name);
            });
  for (const IdlTypeInfo* member : interfaces) {
    if (PerIsolateData::From(isolate)->HasInstance(isolate, *member->interface,
                                                   value)) {
      return value;
    }
  }
  const auto kind_is = [&](IdlTypeKind kind) {
    return find([kind](const IdlTypeInfo& t) { return t.kind == kind; });
  };
  if (const IdlTypeInfo* const member = kind_is(IdlTypeKind::kArrayBuffer);
      member && (value->IsArrayBuffer() || value->IsSharedArrayBuffer())) {
    return convert(member);
  }
  if (const IdlTypeInfo* const member = kind_is(IdlTypeKind::kArrayBufferView);
      member && value->IsArrayBufferView()) {
    return convert(member);
  }
  if (const IdlTypeInfo* const member = kind_is(IdlTypeKind::kDataView);
      member && value->IsDataView()) {
    return convert(member);
  }
  for (const IdlTypeInfo* member : members) {
    if (member->kind == IdlTypeKind::kTypedArray &&
        IsTypedArrayOf(value, member->name)) {
      return convert(member);
    }
  }
  if (const IdlTypeInfo* const member = kind_is(IdlTypeKind::kCallbackFunction);
      member && value->IsFunction()) {
    return convert(member);
  }
  if (const IdlTypeInfo* const member = find([](const IdlTypeInfo& t) {
        return t.kind == IdlTypeKind::kSequence ||
               t.kind == IdlTypeKind::kFrozenArray;
      })) {
    if (value->IsArray()) {
      return convert(member);
    }
    if (value->IsObject()) {
      v8::Local<v8::Function> method;
      if (!GetIteratorMethod(isolate, value.As<v8::Object>(), &method)) {
        return {};
      }
      if (!method.IsEmpty()) {
        return ConvertSequence(isolate, *member, value, method,
                               exception_state);
      }
    }
  }
  if (value->IsObject()) {
    for (const IdlTypeKind kind :
         {IdlTypeKind::kDictionary, IdlTypeKind::kRecord,
          IdlTypeKind::kCallbackInterface, IdlTypeKind::kObject}) {
      if (const IdlTypeInfo* const member = kind_is(kind)) {
        return convert(member);
      }
    }
  }
  if (const IdlTypeInfo* const member = kind_is(IdlTypeKind::kBoolean);
      member && value->IsBoolean()) {
    return convert(member);
  }
  if (const IdlTypeInfo* const member =
          find([](const IdlTypeInfo& t) { return IsNumeric(t.kind); });
      member && value->IsNumber()) {
    return convert(member);
  }
  if (const IdlTypeInfo* const member = kind_is(IdlTypeKind::kBigInt);
      member && value->IsBigInt()) {
    return convert(member);
  }
  if (const IdlTypeInfo* const member = find([](const IdlTypeInfo& t) {
        return t.kind == IdlTypeKind::kEnumeration || IsString(t.kind);
      })) {
    return convert(member);
  }
  if (const IdlTypeInfo* const member =
          find([](const IdlTypeInfo& t) { return IsNumeric(t.kind); })) {
    return convert(member);
  }
  if (const IdlTypeInfo* const member = kind_is(IdlTypeKind::kBoolean)) {
    return convert(member);
  }
  exception_state.ThrowTypeError(ExceptionMessages::ValueNotOfType(type.name));
  return {};
}

v8::MaybeLocal<v8::Value> Convert(v8::Isolate* isolate,
                                  const IdlTypeInfo& type,
                                  v8::Local<v8::Value> value,
                                  int argument_index,
                                  ExceptionState& exception_state) {
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  if ((type.flags & kIdlTypeNullable) && type.kind != IdlTypeKind::kUnion &&
      value->IsNullOrUndefined()) {
    return v8::Null(isolate);
  }
  switch (type.kind) {
    case IdlTypeKind::kAny:
      return value;
    case IdlTypeKind::kUndefined:
      return v8::Undefined(isolate);
    case IdlTypeKind::kBoolean:
      return v8::Boolean::New(isolate, value->BooleanValue(isolate));
    case IdlTypeKind::kByte:
    case IdlTypeKind::kOctet:
    case IdlTypeKind::kShort:
    case IdlTypeKind::kUnsignedShort:
    case IdlTypeKind::kLong:
    case IdlTypeKind::kUnsignedLong:
    case IdlTypeKind::kLongLong:
    case IdlTypeKind::kUnsignedLongLong:
      return ConvertInteger(isolate, type, value, exception_state);
    case IdlTypeKind::kFloat:
    case IdlTypeKind::kUnrestrictedFloat:
    case IdlTypeKind::kDouble:
    case IdlTypeKind::kUnrestrictedDouble:
      return ConvertFloatingPoint(isolate, type, value, exception_state);
    case IdlTypeKind::kBigInt: {
      v8::Local<v8::BigInt> bigint;
      if (!value->ToBigInt(context).ToLocal(&bigint)) {
        return {};
      }
      return bigint;
    }
    case IdlTypeKind::kDOMString:
    case IdlTypeKind::kByteString:
    case IdlTypeKind::kUSVString:
      return ConvertString(isolate, type, value, exception_state);
    case IdlTypeKind::kObject:
      if (value->IsObject()) {
        return value;
      }
      exception_state.ThrowTypeError(NotOfType(argument_index, "object"));
      return {};
    case IdlTypeKind::kSymbol:
      if (value->IsSymbol()) {
        return value;
      }
      exception_state.ThrowTypeError(NotOfType(argument_index, "symbol"));
      return {};
    case IdlTypeKind::kArrayBuffer:
    case IdlTypeKind::kSharedArrayBuffer:
    case IdlTypeKind::kArrayBufferView:
    case IdlTypeKind::kDataView:
    case IdlTypeKind::kTypedArray:
      return ConvertBuffer(type, value, argument_index, exception_state);
    case IdlTypeKind::kInterface:
      if (type.interface && PerIsolateData::From(isolate)->HasInstance(
                                isolate, *type.interface, value)) {
        return value;
      }
      exception_state.ThrowTypeError(NotOfType(argument_index, type.name));
      return {};
    case IdlTypeKind::kCallbackInterface:
      if (value->IsObject()) {
        return value;
      }
      exception_state.ThrowTypeError(
          argument_index >= 0
              ? ExceptionMessages::ArgumentNotOfType(argument_index, "Object")
              : "The given value is not an object.");
      return {};
    case IdlTypeKind::kCallbackFunction:
      if (value->IsFunction()) {
        return value;
      }
      if (type.flags & kIdlTypeLegacyTreatNonObjectAsNull) {
        // JSEventHandler::CreateOrNull(): any object, or else null.
        if (value->IsObject()) {
          return value;
        }
        return v8::Null(isolate);
      }
      exception_state.ThrowTypeError(
          argument_index >= 0
              ? ExceptionMessages::ArgumentNotOfType(argument_index, "Function")
              : "The given value is not a function.");
      return {};
    case IdlTypeKind::kDictionary:
      return ConvertDictionary(isolate, type, value, exception_state);
    case IdlTypeKind::kEnumeration:
      return ConvertEnumeration(isolate, type, value, exception_state);
    case IdlTypeKind::kSequence:
    case IdlTypeKind::kFrozenArray:
    case IdlTypeKind::kObservableArray:
      return ConvertSequence(isolate, type, value, {}, exception_state);
    case IdlTypeKind::kRecord:
      return ConvertRecord(isolate, type, value, exception_state);
    case IdlTypeKind::kPromise: {
      v8::Local<v8::Promise::Resolver> resolver;
      if (!v8::Promise::Resolver::New(context).ToLocal(&resolver) ||
          resolver->Resolve(context, value).IsNothing()) {
        return {};
      }
      return resolver->GetPromise();
    }
    case IdlTypeKind::kUnion:
      return ConvertUnion(isolate, type, value, exception_state);
    case IdlTypeKind::kIterator:
      // No argument is an iterator.
      NOTREACHED();
  }
  NOTREACHED();
}

// One item of Web IDL's effective overload set: an overload, and how many
// arguments it takes there.
struct Effective {
  // RAW_PTR_EXCLUSION: #global-scope, an entry of the generated tables.
  RAW_PTR_EXCLUSION const OverloadInfo* overload;
  size_t size;
};

size_t RequiredOf(const OverloadInfo& overload) {
  size_t required = 0;
  for (const ArgumentInfo& argument : overload.arguments) {
    if (!(argument.flags & (kArgumentOptional | kArgumentVariadic))) {
      ++required;
    }
  }
  return required;
}

// The type of an item's argument at `index`, the variadic one past its end.
const ArgumentInfo& ArgumentAt(const OverloadInfo& overload, size_t index) {
  return overload.arguments[std::min(index, overload.arguments.size() - 1)];
}

// The effective overload set, as web_idl's OverloadGroup computes it.
std::vector<Effective> EffectiveOverloadSet(
    const std::vector<const OverloadInfo*>& overloads) {
  size_t maxarg = 0;
  size_t longest_without_variadic = 0;
  bool any_without_variadic = false;
  for (const OverloadInfo* overload : overloads) {
    maxarg = std::max(maxarg, overload->arguments.size());
    const bool variadic =
        !overload->arguments.empty() &&
        (overload->arguments.back().flags & kArgumentVariadic);
    if (!variadic) {
      any_without_variadic = true;
      longest_without_variadic =
          std::max(longest_without_variadic, overload->arguments.size());
    }
  }
  const size_t n = 1 + (any_without_variadic ? longest_without_variadic : 0);
  std::vector<Effective> set;
  for (const OverloadInfo* overload : overloads) {
    const size_t size = overload->arguments.size();
    set.push_back({overload, size});
    if (size > 0 && (overload->arguments.back().flags & kArgumentVariadic)) {
      for (size_t i = size; i < std::max(maxarg, n); ++i) {
        set.push_back({overload, i + 1});
      }
    }
    for (size_t i = size; i-- > 0;) {
      if (!(overload->arguments[i].flags &
            (kArgumentOptional | kArgumentVariadic))) {
        break;
      }
      set.push_back({overload, i});
    }
  }
  return set;
}

bool IncludesNullableOrDictionary(const IdlTypeInfo& type) {
  if ((type.flags & kIdlTypeNullable) ||
      type.kind == IdlTypeKind::kDictionary) {
    return true;
  }
  if (type.kind != IdlTypeKind::kUnion) {
    return false;
  }
  for (const IdlTypeInfo* member : type.members) {
    if (IncludesNullableOrDictionary(*member)) {
      return true;
    }
  }
  return false;
}

// Whether the type, or a member of the union it is, passes `test`.
template <typename Test>
bool TypeMatches(const IdlTypeInfo& type, Test test) {
  if (test(type)) {
    return true;
  }
  if (type.kind != IdlTypeKind::kUnion) {
    return false;
  }
  std::vector<const IdlTypeInfo*> members;
  bool nullable = false;
  Flatten(type, members, nullable);
  for (const IdlTypeInfo* member : members) {
    if (test(*member)) {
      return true;
    }
  }
  return false;
}

// bind_gen's _make_overload_dispatcher_per_arg_size(): the item that the value
// at the distinguishing argument selects among items of one size, with the
// tests in its order, or null if none does. Returns false if a test threw.
bool Select(v8::Isolate* isolate,
            const std::vector<Effective>& items,
            v8::Local<v8::Value> value,
            size_t index,
            const OverloadInfo** chosen) {
  const auto find = [&](auto test) -> const OverloadInfo* {
    for (const Effective& item : items) {
      const ArgumentInfo& argument = ArgumentAt(*item.overload, index);
      if (test(argument, *argument.type)) {
        return item.overload;
      }
    }
    return nullptr;
  };
  const auto find_type = [&](auto test) {
    return find([&](const ArgumentInfo&, const IdlTypeInfo& type) {
      return TypeMatches(type, test);
    });
  };
  if (value->IsUndefined()) {
    if (const OverloadInfo *const found =
            find([](const ArgumentInfo& argument, const IdlTypeInfo&) {
              return (argument.flags & kArgumentOptional) != 0;
            })) {
      *chosen = found;
      return true;
    }
  }
  if (value->IsNullOrUndefined()) {
    if (const OverloadInfo* const found =
            find([](const ArgumentInfo&, const IdlTypeInfo& type) {
              return IncludesNullableOrDictionary(type);
            })) {
      *chosen = found;
      return true;
    }
  }
  if (value->IsObject()) {
    // Platform objects, from the most derived interface to the least.
    std::vector<std::pair<const WrapperTypeInfo*, const OverloadInfo*>>
        interfaces;
    for (const Effective& item : items) {
      const IdlTypeInfo& type = *ArgumentAt(*item.overload, index).type;
      TypeMatches(type, [&](const IdlTypeInfo& t) {
        if (t.kind == IdlTypeKind::kInterface && t.interface) {
          interfaces.emplace_back(t.interface, item.overload);
        }
        return false;
      });
    }
    std::stable_sort(interfaces.begin(), interfaces.end(),
                     [](const auto& a, const auto& b) {
                       const int depth_a = Depth(a.first);
                       const int depth_b = Depth(b.first);
                       if (depth_a != depth_b) {
                         return depth_a > depth_b;
                       }
                       return std::string_view(a.first->interface_name) >
                              std::string_view(b.first->interface_name);
                     });
    for (const auto& [info, overload] : interfaces) {
      if (PerIsolateData::From(isolate)->HasInstance(isolate, *info, value)) {
        *chosen = overload;
        return true;
      }
    }
    const auto kind_test = [](IdlTypeKind kind) {
      return [kind](const IdlTypeInfo& t) { return t.kind == kind; };
    };
    const OverloadInfo* found = nullptr;
    if ((value->IsArrayBuffer() || value->IsSharedArrayBuffer()) &&
        (found = find_type(kind_test(IdlTypeKind::kArrayBuffer)))) {
      *chosen = found;
      return true;
    }
    if (value->IsArrayBufferView() &&
        (found = find_type(kind_test(IdlTypeKind::kArrayBufferView)))) {
      *chosen = found;
      return true;
    }
    if (value->IsDataView() &&
        (found = find_type(kind_test(IdlTypeKind::kDataView)))) {
      *chosen = found;
      return true;
    }
    if ((found = find_type([&](const IdlTypeInfo& t) {
           return t.kind == IdlTypeKind::kTypedArray &&
                  IsTypedArrayOf(value, t.name);
         }))) {
      *chosen = found;
      return true;
    }
    if (value->IsFunction() &&
        (found = find_type(kind_test(IdlTypeKind::kCallbackFunction)))) {
      *chosen = found;
      return true;
    }
    if ((found = find_type([](const IdlTypeInfo& t) {
           return t.kind == IdlTypeKind::kSequence ||
                  t.kind == IdlTypeKind::kFrozenArray;
         }))) {
      bool iterable = value->IsArray();
      if (!iterable) {
        v8::Local<v8::Function> method;
        if (!GetIteratorMethod(isolate, value.As<v8::Object>(), &method)) {
          return false;
        }
        iterable = !method.IsEmpty();
      }
      if (iterable) {
        *chosen = found;
        return true;
      }
    }
    if ((found = find_type([](const IdlTypeInfo& t) {
           return t.kind == IdlTypeKind::kCallbackInterface ||
                  t.kind == IdlTypeKind::kDictionary ||
                  t.kind == IdlTypeKind::kRecord ||
                  t.kind == IdlTypeKind::kObject;
         }))) {
      *chosen = found;
      return true;
    }
  }
  const OverloadInfo* found = nullptr;
  if (value->IsBoolean() && (found = find_type([](const IdlTypeInfo& t) {
                               return t.kind == IdlTypeKind::kBoolean;
                             }))) {
    *chosen = found;
    return true;
  }
  if (value->IsNumber() && (found = find_type([](const IdlTypeInfo& t) {
                              return IsNumeric(t.kind);
                            }))) {
    *chosen = found;
    return true;
  }
  for (const auto test :
       {+[](const IdlTypeInfo& t) {
          return t.kind == IdlTypeKind::kEnumeration;
        },
        +[](const IdlTypeInfo& t) { return IsString(t.kind); },
        +[](const IdlTypeInfo& t) { return IsNumeric(t.kind); },
        +[](const IdlTypeInfo& t) { return t.kind == IdlTypeKind::kBoolean; },
        +[](const IdlTypeInfo& t) { return t.kind == IdlTypeKind::kAny; }}) {
    if ((found = find_type(test))) {
      *chosen = found;
      return true;
    }
  }
  *chosen = nullptr;
  return true;
}

// Converts the arguments of the chosen overload, each in turn: an optional one
// that is missing or undefined takes its default unconverted, and a variadic
// one each remaining argument.
bool ConvertOverload(const v8::FunctionCallbackInfo<v8::Value>& info,
                     const OverloadInfo& overload,
                     std::vector<v8::Local<v8::Value>>& converted,
                     ExceptionState& exception_state) {
  v8::Isolate* const isolate = info.GetIsolate();
  const int length = info.Length();
  for (size_t i = 0; i < overload.arguments.size(); ++i) {
    const ArgumentInfo& argument = overload.arguments[i];
    if (argument.flags & kArgumentVariadic) {
      for (int j = static_cast<int>(i); j < length; ++j) {
        v8::Local<v8::Value> value;
        if (!Convert(isolate, *argument.type, info[j], j, exception_state)
                 .ToLocal(&value)) {
          return false;
        }
        converted.push_back(value);
      }
      return true;
    }
    const int index = static_cast<int>(i);
    if ((argument.flags & kArgumentOptional) &&
        (index >= length || info[index]->IsUndefined())) {
      converted.push_back(v8::Undefined(isolate));
      continue;
    }
    v8::Local<v8::Value> value;
    if (!Convert(isolate, *argument.type, info[index], index, exception_state)
             .ToLocal(&value)) {
      return false;
    }
    converted.push_back(value);
  }
  return true;
}

}  // namespace

bool IsEnumerationValue(v8::Isolate* isolate,
                        const IdlTypeInfo& type,
                        v8::Local<v8::String> string) {
  const std::string text = Utf8Of(isolate, string);
  for (const char* const item : type.values) {
    if (text == item) {
      return true;
    }
  }
  return false;
}

v8::MaybeLocal<v8::Value> ConvertToIdl(v8::Isolate* isolate,
                                       const IdlTypeInfo& type,
                                       v8::Local<v8::Value> value,
                                       int argument_index,
                                       ExceptionState& exception_state) {
  return Convert(isolate, type, value, argument_index, exception_state);
}

const OverloadInfo* ConvertArguments(
    const v8::FunctionCallbackInfo<v8::Value>& info,
    const OperationInfo& operation,
    std::vector<v8::Local<v8::Value>>& converted,
    ExceptionState& exception_state) {
  v8::Isolate* const isolate = info.GetIsolate();
  const PerIsolateData& data = *PerIsolateData::From(isolate);
  std::vector<const OverloadInfo*> overloads;
  for (const OverloadInfo& overload : operation.overloads) {
    if (!overload.condition || overload.condition(data)) {
      overloads.push_back(&overload);
    }
  }
  CHECK(!overloads.empty());
  const size_t length = static_cast<size_t>(info.Length());
  size_t minimum = RequiredOf(*overloads[0]);
  for (const OverloadInfo* overload : overloads) {
    minimum = std::min(minimum, RequiredOf(*overload));
  }
  const OverloadInfo* chosen = nullptr;
  if (overloads.size() == 1) {
    if (length >= minimum) {
      chosen = overloads[0];
    }
  } else {
    // bind_gen's make_overload_dispatcher(): the items of the effective
    // overload set that take as many arguments as the call gives, or as many
    // as any takes.
    const std::vector<Effective> set = EffectiveOverloadSet(overloads);
    size_t largest = 0;
    for (const Effective& item : set) {
      largest = std::max(largest, item.size);
    }
    const size_t count = std::min(length, largest);
    std::vector<Effective> items;
    for (const Effective& item : set) {
      if (item.size == count) {
        items.push_back(item);
      }
    }
    if (items.size() == 1) {
      chosen = items[0].overload;
    } else if (items.size() > 1) {
      // The distinguishing argument: the first whose types differ, which a
      // valid set of overloads can tell apart there.
      size_t index = 0;
      while (index + 1 < count &&
             ArgumentAt(*items[0].overload, index).type ==
                 ArgumentAt(*items[1].overload, index).type) {
        ++index;
      }
      if (!Select(isolate, items, info[static_cast<int>(index)], index,
                  &chosen)) {
        return nullptr;
      }
    }
  }
  if (!chosen) {
    if (length < minimum) {
      exception_state.ThrowTypeError(ExceptionMessages::NotEnoughArguments(
          static_cast<unsigned>(minimum), static_cast<unsigned>(length)));
    } else {
      exception_state.ThrowTypeError("Overload resolution failed.");
    }
    return nullptr;
  }
  if (!ConvertOverload(info, *chosen, converted, exception_state)) {
    return nullptr;
  }
  return chosen;
}

}  // namespace mitto::bindings
