// Installs an interface's members on V8's templates, as Blink installs them:
// the functions of an attribute's getter and setter are named "get x" and
// "set x", with lengths 0 and 1; an operation's function has the operation's
// length; a constant goes on the prototype and then on the interface object,
// read-only and not configurable. The generated bindings call these in the
// order of the install plan.
//
// Upstream: third_party/blink/renderer/platform/bindings/idl_member_installer.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/generated_code_helper.cc

#ifndef MITTO_BINDINGS_IDL_MEMBER_INSTALLER_H_
#define MITTO_BINDINGS_IDL_MEMBER_INSTALLER_H_

#include <stdint.h>

#include "mitto/bindings/wrapper_type_info.h"
#include "v8/include/v8-function-callback.h"
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

void InstallConstant(v8::Isolate* isolate,
                     v8::Local<v8::Template> target,
                     const char* name,
                     int64_t value);

// Puts the interface object on the global object of `context`, writable,
// configurable and not enumerable, as Blink's exposed constructs are.
void InstallInterfaceObject(v8::Local<v8::Context> context,
                            const char* name,
                            const WrapperTypeInfo& info);

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_IDL_MEMBER_INSTALLER_H_
