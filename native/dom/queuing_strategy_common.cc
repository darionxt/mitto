#include "mitto/dom/queuing_strategy_common.h"

#include <tuple>

#include "v8/include/v8-context.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

v8::Local<v8::Value> GetCachedSizeFunction(v8::Isolate* isolate,
                                           const char* cache_name,
                                           SizeFunctionFactory factory) {
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::Local<v8::Private> key = v8::Private::ForApi(
      isolate, v8::String::NewFromUtf8(isolate, cache_name).ToLocalChecked());
  const v8::Local<v8::Object> global_proxy = context->Global();
  v8::Local<v8::Value> function;
  if (!global_proxy->GetPrivate(context, key).ToLocal(&function) ||
      function->IsUndefined()) {
    function = factory(isolate);
    std::ignore = global_proxy->SetPrivate(context, key, function);
  }
  return function;
}

}  // namespace mitto
