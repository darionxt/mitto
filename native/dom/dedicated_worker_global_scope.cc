#include "mitto/dom/dedicated_worker_global_scope.h"

#include <utility>

#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_dedicated_worker_global_scope.h"

namespace mitto {

// static
DedicatedWorkerGlobalScope* DedicatedWorkerGlobalScope::Create(
    std::u16string name) {
  return bindings::MakeGarbageCollected<DedicatedWorkerGlobalScope>(
      std::move(name));
}

DedicatedWorkerGlobalScope::DedicatedWorkerGlobalScope(std::u16string name)
    : name_(std::move(name)) {}

DedicatedWorkerGlobalScope::~DedicatedWorkerGlobalScope() = default;

const bindings::WrapperTypeInfo*
DedicatedWorkerGlobalScope::GetWrapperTypeInfo() const {
  return &bindings::V8DedicatedWorkerGlobalScope::kWrapperTypeInfo;
}

}  // namespace mitto
