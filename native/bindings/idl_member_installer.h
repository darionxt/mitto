// Installs an interface's members on V8's templates, as Blink installs them:
// the functions of an attribute's getter and setter are named "get x" and
// "set x", with lengths 0 and 1; an operation's function has the operation's
// length; a constant goes on the prototype and then on the interface object,
// read-only and not configurable. The generated bindings call these in the
// order of the install plan.
//
// Upstream: third_party/blink/renderer/platform/bindings/idl_member_installer.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/generated_code_helper.cc
// Upstream: third_party/blink/renderer/platform/bindings/v8_object_constructor.cc

#ifndef MITTO_BINDINGS_IDL_MEMBER_INSTALLER_H_
#define MITTO_BINDINGS_IDL_MEMBER_INSTALLER_H_

#include <stdint.h>

#include <string_view>

#include "base/containers/span.h"
#include "mitto/bindings/wrapper_type_info.h"
#include "v8/include/v8-function-callback.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-template.h"

namespace mitto::bindings {

// Makes the template inherit from its parent's, names it and gives its
// prototype @@toStringTag, as Blink's SetupIDLInterfaceTemplate() does.
void SetUpInterfaceTemplate(v8::Isolate* isolate,
                            v8::Local<v8::FunctionTemplate> interface_template,
                            const WrapperTypeInfo* parent,
                            const char* interface_name);

// Makes the interface's prototype inherit from %Error.prototype%, as Web IDL
// says for DOMException.
void InheritFromErrorPrototype(
    v8::Isolate* isolate,
    v8::Local<v8::FunctionTemplate> interface_template);

void InstallConstructor(v8::Local<v8::FunctionTemplate> interface_template,
                        v8::FunctionCallback callback,
                        int length);

// For an interface without a constructor: `callback` throws "Illegal
// constructor" without the context of a call, as Blink's
// V8ObjectConstructor::IsValidConstructorMode() does.
void InstallIllegalConstructor(
    v8::Local<v8::FunctionTemplate> interface_template,
    v8::FunctionCallback callback);

// `setter` is null for a read-only attribute.
void InstallAttribute(v8::Isolate* isolate,
                      v8::Local<v8::Template> target,
                      v8::Local<v8::Signature> signature,
                      const char* name,
                      const char* interface_name,
                      v8::FunctionCallback getter,
                      v8::FunctionCallback setter,
                      v8::PropertyAttribute attributes,
                      v8::SideEffectType getter_side_effect);

void InstallOperation(v8::Isolate* isolate,
                      v8::Local<v8::Template> target,
                      v8::Local<v8::Signature> signature,
                      const char* name,
                      const char* interface_name,
                      v8::FunctionCallback callback,
                      int length,
                      v8::PropertyAttribute attributes,
                      v8::SideEffectType side_effect);

// The same, on an object that a context made, for what the context decides.
void InstallAttributeOnObject(v8::Isolate* isolate,
                              v8::Local<v8::Context> context,
                              v8::Local<v8::Object> target,
                              v8::Local<v8::Signature> signature,
                              const char* name,
                              const char* interface_name,
                              v8::FunctionCallback getter,
                              v8::FunctionCallback setter,
                              v8::PropertyAttribute attributes,
                              v8::SideEffectType getter_side_effect);
void InstallOperationOnObject(v8::Isolate* isolate,
                              v8::Local<v8::Context> context,
                              v8::Local<v8::Object> target,
                              v8::Local<v8::Signature> signature,
                              const char* name,
                              const char* interface_name,
                              v8::FunctionCallback callback,
                              int length,
                              v8::PropertyAttribute attributes,
                              v8::SideEffectType side_effect);

// Gives the prototype its @@unscopables, an object with a null prototype that
// has each name set to true, as Web IDL says for members with [Unscopable].
void InstallUnscopablePropertyNames(
    v8::Isolate* isolate,
    v8::Local<v8::Context> context,
    v8::Local<v8::Object> prototype_object,
    base::span<const char* const> property_names);

// Deletes the `constructor` that V8 gives every prototype a template makes,
// which an interface with [LegacyNoInterfaceObject] has not.
void DeleteConstructorProperty(v8::Isolate* isolate,
                               v8::Local<v8::Context> context,
                               v8::Local<v8::Object> prototype_object);

void InstallConstant(v8::Isolate* isolate,
                     v8::Local<v8::Template> target,
                     const char* name,
                     int64_t value);

// The interface object in `context`, whose own prototype is its parent's
// interface object, if it has a parent, or the nearest ancestor's that has
// one.
v8::Local<v8::Function> InterfaceObject(v8::Local<v8::Context> context,
                                        const WrapperTypeInfo& info);

// Makes a promise rejected with a TypeError of `message` the call's result, as
// Blink's bindings answer a call of an operation that returns a promise and
// fails, and its wrong receiver too.
void RejectWithTypeError(const v8::FunctionCallbackInfo<v8::Value>& info,
                         std::string_view message);

// Puts the interface object on the global object of `context`, writable,
// configurable and not enumerable, as Blink's exposed constructs are.
void InstallInterfaceObject(v8::Local<v8::Context> context,
                            const char* name,
                            const WrapperTypeInfo& info);

// Installs what `context` decides on the interface's objects in it, and on
// `instance_object`, the global object, for a global interface.
void InstallContextDependentPropertiesOf(v8::Local<v8::Context> context,
                                         const WrapperTypeInfo& info,
                                         v8::Local<v8::Object> instance_object);

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_IDL_MEMBER_INSTALLER_H_
