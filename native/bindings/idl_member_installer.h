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
#include "base/memory/raw_ptr_exclusion.h"
#include "mitto/bindings/wrapper_type_info.h"
#include "v8/include/v8-function-callback.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-template.h"

namespace mitto::bindings {

struct OperationInfo;

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
                        int length,
                        v8::Local<v8::Value> data = {});

// For an interface without a constructor: `callback` throws "Illegal
// constructor" without the context of a call, as Blink's
// V8ObjectConstructor::IsValidConstructorMode() does.
void InstallIllegalConstructor(
    v8::Local<v8::FunctionTemplate> interface_template,
    v8::FunctionCallback callback);

// `setter` is null for a read-only attribute. `data` is what the callbacks get
// as their data, such as the member's entry of an interface table.
void InstallAttribute(v8::Isolate* isolate,
                      v8::Local<v8::Template> target,
                      v8::Local<v8::Signature> signature,
                      const char* name,
                      const char* interface_name,
                      v8::FunctionCallback getter,
                      v8::FunctionCallback setter,
                      v8::PropertyAttribute attributes,
                      v8::SideEffectType getter_side_effect,
                      v8::Local<v8::Value> data = {});

void InstallOperation(v8::Isolate* isolate,
                      v8::Local<v8::Template> target,
                      v8::Local<v8::Signature> signature,
                      const char* name,
                      const char* interface_name,
                      v8::FunctionCallback callback,
                      int length,
                      v8::PropertyAttribute attributes,
                      v8::SideEffectType side_effect,
                      v8::Local<v8::Value> data = {});

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
                              v8::SideEffectType getter_side_effect,
                              v8::Local<v8::Value> data = {});
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
                              v8::Local<v8::Value> data = {});

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
void InstallConstantOnObject(v8::Isolate* isolate,
                             v8::Local<v8::Context> context,
                             v8::Local<v8::Object> target,
                             const char* name,
                             int64_t value);

// The interface object of an interface or a callback interface in `context`,
// which the context makes once, as Blink's V8PerContextData::
// ConstructorForType() does: its own prototype is its parent's interface
// object, if it has a parent, or the nearest ancestor's that has one, and
// what the context decides is installed on it, its prototype and, for a
// global interface, the global object, before anything else can reach them.
v8::Local<v8::Function> InterfaceObject(v8::Local<v8::Context> context,
                                        const WrapperTypeInfo& info);

// A new namespace object in `context`, with what the context decides, as
// Blink's GetExposedNamespaceObject() makes one each time it is asked.
v8::Local<v8::Object> NamespaceObject(v8::Local<v8::Context> context,
                                      const WrapperTypeInfo& info);

// Makes a promise rejected with a TypeError of `message` the call's result, as
// Blink's bindings answer a call of an operation that returns a promise and
// fails, and its wrong receiver too.
void RejectWithTypeError(const v8::FunctionCallbackInfo<v8::Value>& info,
                         std::string_view message);

// A property of a global object that gives a construct exposed on it.
struct ExposedConstructInfo {
  enum class Kind : uint8_t {
    // The interface object of an interface or a callback interface, under its
    // own name or a [LegacyWindowAlias].
    kInterfaceObject,
    kNamespaceObject,
    // A [LegacyFactoryFunction] of the interface, such as Image.
    kFactoryFunction,
  };

  const char* name;
  // RAW_PTR_EXCLUSION: #global-scope, as in WrapperTypeInfo.
  RAW_PTR_EXCLUSION const WrapperTypeInfo* target;
  Kind kind;
  // A factory function's length.
  int16_t length;
  // A factory function's overloads, and its number as a Known Gap.
  RAW_PTR_EXCLUSION const OperationInfo* operation;
  uint16_t number;
  // Whether the factory function keeps today's error, as a refused Known Gap.
  bool refused;
  // The callback of a factory function that has its behaviour, which the
  // bindings of its interface's class write, or null for a Known Gap.
  v8::FunctionCallback callback = nullptr;
};

// Puts the property on a global object's template, or on a context's global
// object, as a lazy data property that is not enumerable, as Blink's
// IDLMemberInstaller::InstallExposedConstructs() does: V8 makes the object the
// first time a page reads it, and the property is a data property from then
// on. V8 installs a template's lazy data properties before its others, last
// first, as Chrome's window shows.
void InstallExposedConstruct(v8::Isolate* isolate,
                             v8::Local<v8::ObjectTemplate> instance_template,
                             const ExposedConstructInfo& construct);
void InstallExposedConstructOnObject(v8::Isolate* isolate,
                                     v8::Local<v8::Context> context,
                                     v8::Local<v8::Object> instance_object,
                                     const ExposedConstructInfo& construct);

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_IDL_MEMBER_INSTALLER_H_
