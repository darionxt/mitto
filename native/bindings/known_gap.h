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
// An event handler attribute stores and gives what Chrome's do, as a listener
// of the EventTarget behind its object (PerIsolateData::ObjectHooks), and
// reports nothing. A member that the generator refuses keeps today's error,
// `TypeError: Not implemented.`.

#ifndef MITTO_BINDINGS_KNOWN_GAP_H_
#define MITTO_BINDINGS_KNOWN_GAP_H_

#include <stdint.h>

#include <vector>

#include "mitto/bindings/idl_type_info.h"
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
// The constructor of an interface with a class that a Known Gap may make,
// where the class lacks the constructor's behaviour: its bindings call it with
// the constructor's overloads and number. It makes the class's object with
// nothing given behind the object V8 made for `new`.
void KnownGapConstructorOfClass(const v8::FunctionCallbackInfo<v8::Value>& info,
                                const WrapperTypeInfo& interface,
                                const OperationInfo& operation,
                                uint16_t number);
// next() and return() of an iterator over nothing, which a Known Gap gives:
// { value: undefined, done: true }, or for return() the value it gets, and for
// an async iterator a promise resolved with it, as Blink's iterators give once
// they are done. The next() of an iterator over a list gives each of its items
// in turn first, { value: item, done: false }.
void IteratorStep(const v8::FunctionCallbackInfo<v8::Value>& info);

// An iterator of `iterator`, the iterator interface of a maplike or setlike
// declaration, over `items`, which its next() gives in order, as Blink's
// iterators give what their iteration source took when they were made.
v8::MaybeLocal<v8::Value> NewIteratorOverList(
    v8::Local<v8::Context> context,
    const WrapperTypeInfo& iterator,
    const std::vector<v8::Local<v8::Value>>& items);

// Whether a Known Gap may make an object of the interface: one without a C++
// class, which has a table, or one whose class makes objects for Known Gaps.
bool IsSafeObjectType(const WrapperTypeInfo& info);

// An object of an interface in `context` that a Known Gap may make: for one
// without a C++ class, with nothing behind it but, for one that inherits from
// EventTarget, Event or a class that makes objects for Known Gaps, an object of
// that class; for one with a class, the object its class makes with nothing
// given.
v8::MaybeLocal<v8::Value> NewObjectOfInterface(v8::Local<v8::Context> context,
                                               const WrapperTypeInfo& info);

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_KNOWN_GAP_H_
