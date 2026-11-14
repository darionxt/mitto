#include "mitto/bindings/indexed_and_named_properties.h"

#include <string>
#include <tuple>
#include <vector>

#include "base/strings/string_number_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/interface_table.h"
#include "mitto/bindings/v8_conversions.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-external.h"
#include "v8/include/v8-function-callback.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-template.h"

namespace mitto::bindings {

namespace {

template <typename T>
const IndexedAndNamedProperties& PropertiesOf(
    const v8::PropertyCallbackInfo<T>& info) {
  return *static_cast<const IndexedAndNamedProperties*>(
      info.Data().template As<v8::External>()->Value(
          kIndexedAndNamedPropertiesTag));
}

// The property name of an index, for an interface with a named property
// getter but no indexed one, as bind_gen's V8AtomicString of the index.
v8::Local<v8::Name> NameOfIndex(v8::Isolate* isolate, uint32_t index) {
  return ToV8StringFromUtf8(isolate, base::NumberToString(index));
}

std::u16string StringOf(v8::Isolate* isolate, v8::Local<v8::Name> name) {
  // The named interceptors take strings only, which convert without throwing.
  std::u16string result;
  std::ignore = FromV8String(isolate, name, &result);
  return result;
}

// Blink's CreatePropertyDescriptorObject() for a data property, ECMAScript's
// FromPropertyDescriptor().
v8::Local<v8::Object> DataPropertyDescriptor(v8::Isolate* isolate,
                                             v8::Local<v8::Value> value,
                                             bool writable,
                                             bool enumerable) {
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::Local<v8::Object> descriptor = v8::Object::New(isolate);
  const auto add = [&](const char* name, v8::Local<v8::Value> field) {
    return descriptor->CreateDataProperty(
        context, ToV8StringFromUtf8(isolate, name), field);
  };
  bool added;
  if (!add("value", value).To(&added) ||
      !add("writable", v8::Boolean::New(isolate, writable)).To(&added) ||
      !add("enumerable", v8::Boolean::New(isolate, enumerable)).To(&added) ||
      !add("configurable", v8::True(isolate)).To(&added)) {
    return v8::Local<v8::Object>();
  }
  return descriptor;
}

template <typename T>
void ThrowIfShouldThrow(const v8::PropertyCallbackInfo<T>& info,
                        std::string_view message) {
  if (info.ShouldThrowOnError()) {
    ThrowTypeError(info.GetIsolate(), message);
  }
}

// The named interceptors.

v8::Intercepted NamedGetter(v8::Local<v8::Name> name,
                            const v8::PropertyCallbackInfo<v8::Value>& info) {
  // LegacyPlatformObjectGetOwnProperty: if the named property visibility
  // algorithm says P is visible, the value of the named property P. V8 calls
  // a non-masking interceptor only for a name that neither the object nor its
  // prototypes have.
  v8::Isolate* const isolate = info.GetIsolate();
  const v8::Local<v8::Value> value = PropertiesOf(info).named_getter(
      isolate, info.Holder(), StringOf(isolate, name));
  if (value.IsEmpty()) {
    // OrdinaryGetOwnProperty(O, P).
    return v8::Intercepted::kNo;
  }
  info.GetReturnValue().Set(value);
  return v8::Intercepted::kYes;
}

v8::Intercepted NamedSetter(v8::Local<v8::Name> name,
                            v8::Local<v8::Value> value,
                            const v8::PropertyCallbackInfo<v8::Boolean>& info) {
  v8::Isolate* const isolate = info.GetIsolate();
  const IndexedAndNamedProperties& properties = PropertiesOf(info);
  if (properties.named_setter) {
    // Web IDL's [[Set]], step 2: the named property setter, for any string.
    properties.named_setter(isolate, info.Holder(), StringOf(isolate, name),
                            value);
    return v8::Intercepted::kYes;
  }
  // Web IDL's [[Set]] of a legacy platform object without a named property
  // setter: OrdinarySetWithOwnDescriptor(), which fails for a supported
  // property name, whose own property is read-only.
  if ((properties.flags & kNamedPropertiesOverrideBuiltIns) &&
      info.Holder()
          ->GetRealNamedPropertyAttributesInPrototypeChain(
              isolate->GetCurrentContext(), name)
          .IsJust()) {
    return v8::Intercepted::kNo;
  }
  if (properties.has_name(isolate, info.Holder(), StringOf(isolate, name))) {
    ThrowIfShouldThrow(info, "Named property setter is not supported.");
    return v8::Intercepted::kYes;
  }
  // A new own property.
  return v8::Intercepted::kNo;
}

v8::Intercepted NamedQuery(v8::Local<v8::Name> name,
                           const v8::PropertyCallbackInfo<v8::Integer>& info) {
  v8::Isolate* const isolate = info.GetIsolate();
  const IndexedAndNamedProperties& properties = PropertiesOf(info);
  if (!properties.has_name(isolate, info.Holder(), StringOf(isolate, name))) {
    return v8::Intercepted::kNo;
  }
  uint32_t attributes = properties.named_setter ? v8::None : v8::ReadOnly;
  if (properties.flags & kNamedPropertiesUnenumerable) {
    attributes |= v8::DontEnum;
  }
  info.GetReturnValue().Set(attributes);
  return v8::Intercepted::kYes;
}

v8::Intercepted NamedDeleter(
    v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Boolean>& info) {
  v8::Isolate* const isolate = info.GetIsolate();
  const IndexedAndNamedProperties& properties = PropertiesOf(info);
  if (properties.named_deleter) {
    // Web IDL's [[Delete]], step 2.2: the named property deleter, which
    // leaves a property it did not delete to an ordinary delete.
    if (!properties.named_deleter(isolate, info.Holder(),
                                  StringOf(isolate, name))) {
      return v8::Intercepted::kNo;
    }
    info.GetReturnValue().Set(true);
    return v8::Intercepted::kYes;
  }
  // Web IDL's [[Delete]]: without a named property deleter, false for a
  // visible named property.
  if (!properties.has_name(isolate, info.Holder(), StringOf(isolate, name))) {
    return v8::Intercepted::kNo;
  }
  info.GetReturnValue().Set(false);
  ThrowIfShouldThrow(info, "Named property deleter is not supported.");
  return v8::Intercepted::kYes;
}

void NamedEnumerator(const v8::PropertyCallbackInfo<v8::Array>& info) {
  // Web IDL's [[OwnPropertyKeys]], step 3: the supported property names.
  v8::Isolate* const isolate = info.GetIsolate();
  const std::vector<std::u16string> names =
      PropertiesOf(info).names(isolate, info.Holder());
  info.GetReturnValue().Set(ToV8StringArray(isolate, names).As<v8::Array>());
}

v8::Intercepted NamedDefiner(
    v8::Local<v8::Name> name,
    const v8::PropertyDescriptor& descriptor,
    const v8::PropertyCallbackInfo<v8::Boolean>& info) {
  v8::Isolate* const isolate = info.GetIsolate();
  if (PropertiesOf(info).named_setter) {
    // Web IDL's [[DefineOwnProperty]], step 2.2.2: false for an accessor,
    // and otherwise the named property setter with the value.
    if (descriptor.has_get() || descriptor.has_set()) {
      ThrowIfShouldThrow(info, "Accessor properties are not allowed.");
      return v8::Intercepted::kYes;
    }
    return NamedSetter(name,
                       descriptor.has_value()
                           ? descriptor.value()
                           : v8::Undefined(isolate).As<v8::Value>(),
                       info);
  }
  // Web IDL's [[DefineOwnProperty]], step 2: without a named property
  // setter, false for a supported property name.
  if (PropertiesOf(info).has_name(isolate, info.Holder(),
                                  StringOf(isolate, name))) {
    ThrowIfShouldThrow(info, "Named property setter is not supported.");
    return v8::Intercepted::kYes;
  }
  // OrdinaryDefineOwnProperty().
  return v8::Intercepted::kNo;
}

v8::Intercepted NamedDescriptor(
    v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  // LegacyPlatformObjectGetOwnProperty: a named property is visible unless
  // the prototype chain has a property of that name, or the interface has
  // [LegacyOverrideBuiltIns].
  v8::Isolate* const isolate = info.GetIsolate();
  if (!(PropertiesOf(info).flags & kNamedPropertiesOverrideBuiltIns) &&
      info.Holder()
          ->GetRealNamedPropertyAttributesInPrototypeChain(
              isolate->GetCurrentContext(), name)
          .IsJust()) {
    return v8::Intercepted::kNo;
  }
  if (NamedGetter(name, info) == v8::Intercepted::kNo) {
    return v8::Intercepted::kNo;
  }
  // Not writable without a named property setter, and not enumerable for
  // [LegacyUnenumerableNamedProperties].
  const bool enumerable =
      !(PropertiesOf(info).flags & kNamedPropertiesUnenumerable);
  const bool writable = PropertiesOf(info).named_setter;
  info.GetReturnValue().Set(DataPropertyDescriptor(
      isolate, info.GetReturnValue().Get(), writable, enumerable));
  return v8::Intercepted::kYes;
}

// The indexed interceptors. An interface with a named property getter and no
// indexed one passes an index to the named ones as its name.

v8::Intercepted IndexedGetter(uint32_t index,
                              const v8::PropertyCallbackInfo<v8::Value>& info) {
  v8::Isolate* const isolate = info.GetIsolate();
  const IndexedAndNamedProperties& properties = PropertiesOf(info);
  if (!properties.indexed_getter) {
    return NamedGetter(NameOfIndex(isolate, index), info);
  }
  // LegacyPlatformObjectGetOwnProperty, step 1.2: only a supported property
  // index has a value.
  if (index >= properties.length(isolate, info.Holder())) {
    return v8::Intercepted::kNo;
  }
  info.GetReturnValue().Set(
      properties.indexed_getter(isolate, info.Holder(), index));
  return v8::Intercepted::kYes;
}

v8::Intercepted IndexedSetter(
    uint32_t index,
    v8::Local<v8::Value> value,
    const v8::PropertyCallbackInfo<v8::Boolean>& info) {
  const IndexedAndNamedProperties& properties = PropertiesOf(info);
  if (!properties.indexed_getter) {
    return NamedSetter(NameOfIndex(info.GetIsolate(), index), value, info);
  }
  // Web IDL's [[Set]]: the indexed property setter, or else
  // OrdinarySetWithOwnDescriptor() ends in [[DefineOwnProperty]], which fails
  // without one.
  if (properties.indexed_setter) {
    properties.indexed_setter(info.GetIsolate(), info.Holder(), index, value);
    return v8::Intercepted::kYes;
  }
  ThrowIfShouldThrow(info, "Indexed property setter is not supported.");
  return v8::Intercepted::kYes;
}

v8::Intercepted IndexedDeleter(
    uint32_t index,
    const v8::PropertyCallbackInfo<v8::Boolean>& info) {
  v8::Isolate* const isolate = info.GetIsolate();
  const IndexedAndNamedProperties& properties = PropertiesOf(info);
  if (!properties.indexed_getter) {
    return NamedDeleter(NameOfIndex(isolate, index), info);
  }
  // Web IDL's [[Delete]], step 1: true for an index that is not supported,
  // false for one that is.
  const bool is_supported = index < properties.length(isolate, info.Holder());
  info.GetReturnValue().Set(!is_supported);
  if (is_supported) {
    ThrowIfShouldThrow(info, "Index property deleter is not supported.");
  }
  return v8::Intercepted::kYes;
}

void IndexedEnumerator(const v8::PropertyCallbackInfo<v8::Array>& info) {
  // Web IDL's [[OwnPropertyKeys]], step 2: the supported property indices in
  // ascending order, as Blink's EnumerateIndexedProperties() gives them.
  v8::Isolate* const isolate = info.GetIsolate();
  const uint32_t length = PropertiesOf(info).length(isolate, info.Holder());
  v8::LocalVector<v8::Value> indices(isolate);
  indices.reserve(length);
  for (uint32_t i = 0; i < length; ++i) {
    indices.push_back(v8::Integer::NewFromUnsigned(isolate, i));
  }
  info.GetReturnValue().Set(
      v8::Array::New(isolate, indices.data(), indices.size()));
}

v8::Intercepted IndexedDefiner(
    uint32_t index,
    const v8::PropertyDescriptor& descriptor,
    const v8::PropertyCallbackInfo<v8::Boolean>& info) {
  if (!PropertiesOf(info).indexed_getter) {
    return NamedDefiner(NameOfIndex(info.GetIsolate(), index), descriptor,
                        info);
  }
  // Web IDL's [[DefineOwnProperty]], step 1: false for an accessor, and false
  // without an indexed property setter, which otherwise sets the value.
  if (descriptor.has_get() || descriptor.has_set()) {
    ThrowIfShouldThrow(info, "Accessor properties are not allowed.");
    return v8::Intercepted::kYes;
  }
  const IndexedAndNamedProperties& properties = PropertiesOf(info);
  if (properties.indexed_setter) {
    v8::Isolate* const isolate = info.GetIsolate();
    properties.indexed_setter(isolate, info.Holder(), index,
                              descriptor.has_value()
                                  ? descriptor.value()
                                  : v8::Undefined(isolate).As<v8::Value>());
    return v8::Intercepted::kYes;
  }
  ThrowIfShouldThrow(info, "Index property setter is not supported.");
  return v8::Intercepted::kYes;
}

v8::Intercepted IndexedDescriptor(
    uint32_t index,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  v8::Isolate* const isolate = info.GetIsolate();
  if (!PropertiesOf(info).indexed_getter) {
    return NamedDescriptor(NameOfIndex(isolate, index), info);
  }
  if (IndexedGetter(index, info) == v8::Intercepted::kNo) {
    return v8::Intercepted::kNo;
  }
  // LegacyPlatformObjectGetOwnProperty, step 1.2: writable only with an
  // indexed property setter, enumerable and configurable.
  info.GetReturnValue().Set(DataPropertyDescriptor(
      isolate, info.GetReturnValue().Get(),
      /*writable=*/PropertiesOf(info).indexed_setter != nullptr,
      /*enumerable=*/true));
  return v8::Intercepted::kYes;
}

}  // namespace

void InstallIndexedAndNamedProperties(
    v8::Isolate* isolate,
    v8::Local<v8::ObjectTemplate> instance_template,
    const IndexedAndNamedProperties& properties) {
  const v8::Local<v8::External> data = v8::External::New(
      isolate, const_cast<IndexedAndNamedProperties*>(&properties),
      kIndexedAndNamedPropertiesTag);
  if (properties.named_getter) {
    // Named properties are strings, and never shadow a property of the object
    // or its prototypes, without [LegacyOverrideBuiltIns].
    int flags =
        static_cast<int>(v8::PropertyHandlerFlags::kOnlyInterceptStrings);
    if (!(properties.flags & kNamedPropertiesOverrideBuiltIns)) {
      flags |= static_cast<int>(v8::PropertyHandlerFlags::kNonMasking);
    }
    if (!(properties.flags & kNamedGetterHasSideEffects)) {
      flags |= static_cast<int>(v8::PropertyHandlerFlags::kHasNoSideEffect);
    }
    instance_template->SetHandler(v8::NamedPropertyHandlerConfiguration(
        &NamedGetter, &NamedSetter, &NamedQuery, &NamedDeleter,
        &NamedEnumerator, &NamedDefiner, &NamedDescriptor, data,
        static_cast<v8::PropertyHandlerFlags>(flags)));
  }
  const bool indexed_has_no_side_effect =
      properties.indexed_getter &&
      !(properties.flags & kIndexedGetterHasSideEffects);
  instance_template->SetHandler(v8::IndexedPropertyHandlerConfiguration(
      &IndexedGetter, &IndexedSetter, /*query=*/nullptr, &IndexedDeleter,
      properties.indexed_getter ? &IndexedEnumerator : nullptr, &IndexedDefiner,
      &IndexedDescriptor, data,
      indexed_has_no_side_effect ? v8::PropertyHandlerFlags::kHasNoSideEffect
                                 : v8::PropertyHandlerFlags::kNone));
}

}  // namespace mitto::bindings
