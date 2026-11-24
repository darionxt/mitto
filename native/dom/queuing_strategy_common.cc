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
  // On an object of the context's own, as its global proxy is another
  // context's once a frame has navigated.
  const v8::Local<v8::Object> holder = context->GetExtrasBindingObject();
  v8::Local<v8::Value> function;
  if (!holder->GetPrivate(context, key).ToLocal(&function) ||
      function->IsUndefined()) {
    function = factory(isolate);
    std::ignore = holder->SetPrivate(context, key, function);
  }
  return function;
}

}  // namespace mitto
