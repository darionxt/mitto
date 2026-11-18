// What ByteLengthQueuingStrategy and CountQueuingStrategy share: the size
// function, which each global object makes once, as Blink caches it on the
// global proxy.
//
// Upstream: third_party/blink/renderer/core/streams/queuing_strategy_common.h
// Upstream: third_party/blink/renderer/core/streams/queuing_strategy_common.cc

#ifndef MITTO_DOM_QUEUING_STRATEGY_COMMON_H_
#define MITTO_DOM_QUEUING_STRATEGY_COMMON_H_

#include "v8/include/v8-local-handle.h"

namespace v8 {
class Function;
class Isolate;
class Value;
}  // namespace v8

namespace mitto {

using SizeFunctionFactory = v8::Local<v8::Function> (*)(v8::Isolate* isolate);

// The function that the current context's global proxy keeps under
// `cache_name`, which `factory` makes the first time.
v8::Local<v8::Value> GetCachedSizeFunction(v8::Isolate* isolate,
                                           const char* cache_name,
                                           SizeFunctionFactory factory);

}  // namespace mitto

#endif  // MITTO_DOM_QUEUING_STRATEGY_COMMON_H_
