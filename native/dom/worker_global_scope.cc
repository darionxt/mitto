#include "mitto/dom/worker_global_scope.h"

#include "mitto/bindings/v8_worker_global_scope.h"
#include "v8/include/v8-context.h"

namespace mitto {

WorkerGlobalScope::WorkerGlobalScope() = default;
WorkerGlobalScope::~WorkerGlobalScope() = default;

const bindings::WrapperTypeInfo* WorkerGlobalScope::GetWrapperTypeInfo() const {
  return &bindings::V8WorkerGlobalScope::kWrapperTypeInfo;
}

void WorkerGlobalScope::Trace(cppgc::Visitor* visitor) const {
  EventTarget::Trace(visitor);
  ExecutionContext::Trace(visitor);
}

void WorkerGlobalScope::AssociateWithContext(v8::Isolate* isolate,
                                             v8::Local<v8::Context> context) {
  AssociateWithGlobalProxy(isolate, context->Global());
}

}  // namespace mitto
