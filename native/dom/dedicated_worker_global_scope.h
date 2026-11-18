// The HTML Standard's DedicatedWorkerGlobalScope, the global object of a
// dedicated worker's context, with the name that `new Worker()` gave it; its
// other members are Known Gaps.
//
// Upstream: third_party/blink/renderer/core/workers/dedicated_worker_global_scope.h

#ifndef MITTO_DOM_DEDICATED_WORKER_GLOBAL_SCOPE_H_
#define MITTO_DOM_DEDICATED_WORKER_GLOBAL_SCOPE_H_

#include <string>

#include "mitto/dom/worker_global_scope.h"

namespace mitto {

class DedicatedWorkerGlobalScope final : public WorkerGlobalScope {
 public:
  // A new scope of a worker named `name`, on the current isolate.
  static DedicatedWorkerGlobalScope* Create(std::u16string name);

  explicit DedicatedWorkerGlobalScope(std::u16string name);
  ~DedicatedWorkerGlobalScope() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  const std::u16string& Name() const { return name_; }

 private:
  const std::u16string name_;
};

}  // namespace mitto

#endif  // MITTO_DOM_DEDICATED_WORKER_GLOBAL_SCOPE_H_
