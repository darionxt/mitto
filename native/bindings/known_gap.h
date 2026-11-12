// The callbacks of a member of an interface without a C++ class, whose
// behaviour the Runtime does not have: a Known Gap.
//
// Each is a function template's callback with the member's entry of its
// interface table, a MemberInfo, as its data, or for a constructor the
// interface's WrapperTypeInfo. They check the call as Chrome's binding does
// before it reaches Blink, the receiver through V8's signature or as the
// member's flags say, and then throw `TypeError: Not implemented.`, or for an
// operation that returns a promise, give a promise rejected with it, as a
// Known Gap of an interface with a class does (docs/runtime.md, "What a Known
// Gap does").

#ifndef MITTO_BINDINGS_KNOWN_GAP_H_
#define MITTO_BINDINGS_KNOWN_GAP_H_

#include "v8/include/v8-function-callback.h"
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

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_KNOWN_GAP_H_
