// The callbacks of a member whose behaviour the Runtime lacks: a Known Gap.
//
// Each is a function template's callback with the member's MemberInfo as its
// data, or for a constructor the interface's WrapperTypeInfo, or for a
// [LegacyFactoryFunction] its ExposedConstructInfo. They do what the owner
// chose for a Known Gap (docs/runtime.md, "What a Known Gap does"):
//
// 1. Chrome's checks, as the binding does them before it reaches Blink: the
//    receiver, through V8's signature or as the member's flags say, `new` for
//    a constructor, the number of arguments, overload resolution, and each
//    argument converted (idl_conversions.h). For an operation that returns a
//    promise, an error from any of them rejects it.
// 2. A report to the Session, the first time a document reaches the member,
//    as Blink's use counters count a feature.
// 3. The type's default: a getter gives what its setter stored on that
//    object, or its type's default, the same object each time unless the IDL
//    says [NewObject]; a setter stores the converted value; an operation gives
//    its return type's default, a promise resolved with the promised type's;
//    a constructor gives the object V8 made for `new`.
//
// An event handler attribute stores and gives what Chrome's do, and reports
// nothing. A member that the generator refuses keeps today's error,
// `TypeError: Not implemented.`.

#ifndef MITTO_BINDINGS_KNOWN_GAP_H_
#define MITTO_BINDINGS_KNOWN_GAP_H_

#include "mitto/bindings/wrapper_type_info.h"
#include "v8/include/v8-function-callback.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-value.h"

namespace mitto::bindings {

void KnownGapAttributeGetter(const v8::FunctionCallbackInfo<v8::Value>& info);
void KnownGapAttributeSetter(const v8::FunctionCallbackInfo<v8::Value>& info);
void KnownGapOperation(const v8::FunctionCallbackInfo<v8::Value>& info);
// For an operation that returns a promise.
void KnownGapPromiseOperation(const v8::FunctionCallbackInfo<v8::Value>& info);
// The interface object's call handler, with the WrapperTypeInfo as its data.
void KnownGapConstructor(const v8::FunctionCallbackInfo<v8::Value>& info);
// A [LegacyFactoryFunction]'s, with its ExposedConstructInfo as its data.
void KnownGapFactoryFunction(const v8::FunctionCallbackInfo<v8::Value>& info);

// Whether a Known Gap may make an object of the interface: one without a C++
// class, which has a table.
bool IsSafeObjectType(const WrapperTypeInfo& info);

// An object of an interface without a C++ class in `context`, with nothing
// behind it but, for one that inherits from EventTarget or Event, an object of
// that class.
v8::MaybeLocal<v8::Value> NewObjectOfInterface(v8::Local<v8::Context> context,
                                               const WrapperTypeInfo& info);

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_KNOWN_GAP_H_
