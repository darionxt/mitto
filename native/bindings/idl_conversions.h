// Web IDL's conversions of JavaScript values, from the types of
// idl_type_info.h, as Chrome's bindings convert them: what a Known Gap does
// with its arguments before it gives its default. The page's own code runs as
// it runs in Chrome, such as a dictionary's getters, a sequence's iterator,
// valueOf() and toString(), and each failure throws Chrome's error with
// Chrome's message.
//
// Upstream: third_party/blink/renderer/bindings/core/v8/native_value_traits_impl.h
// Upstream: third_party/blink/renderer/bindings/core/v8/native_value_traits_impl.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/v8_binding_for_core.cc
// Upstream: third_party/blink/renderer/bindings/scripts/bind_gen/interface.py
// Upstream: third_party/blink/renderer/bindings/scripts/bind_gen/union.py
// Upstream: third_party/blink/renderer/bindings/scripts/bind_gen/dictionary.py

#ifndef MITTO_BINDINGS_IDL_CONVERSIONS_H_
#define MITTO_BINDINGS_IDL_CONVERSIONS_H_

#include <vector>

#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/idl_type_info.h"
#include "v8/include/v8-function-callback.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-value.h"

namespace mitto::bindings {

// Converts `value` to `type`. Returns the converted value as a page would
// read it back, such as a number in the type's range, a string, a frozen
// array of converted items, or for a dictionary an object with a null
// prototype that holds the members that were present, converted; or an empty
// value if the conversion threw. `argument_index` is the argument's index
// from 0, for the messages that name it, or -1 for a value that is not an
// argument, such as an attribute's.
v8::MaybeLocal<v8::Value> ConvertToIdl(v8::Isolate* isolate,
                                       const IdlTypeInfo& type,
                                       v8::Local<v8::Value> value,
                                       int argument_index,
                                       ExceptionState& exception_state);

// Whether `string` is one of the values of the enumeration `type`.
bool IsEnumerationValue(v8::Isolate* isolate,
                        const IdlTypeInfo& type,
                        v8::Local<v8::String> string);

// Chooses the overload of `operation` that the call's arguments select, as
// Web IDL's overload resolution does, checks the number of arguments, and
// converts each argument in turn into `converted`. Returns the overload, or
// null if anything threw.
const OverloadInfo* ConvertArguments(
    const v8::FunctionCallbackInfo<v8::Value>& info,
    const OperationInfo& operation,
    std::vector<v8::Local<v8::Value>>& converted,
    ExceptionState& exception_state);

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_IDL_CONVERSIONS_H_
