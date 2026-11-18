// The HTML Standard's DedicatedWorkerGlobalScope, the global object of a
// dedicated worker's context, with the name that `new Worker()` gave it, and
// postMessage() and close(), which reach its Worker through the Page
// program's proxy; its other members are Known Gaps.
//
// Upstream: third_party/blink/renderer/core/workers/dedicated_worker_global_scope.h
// Upstream: third_party/blink/renderer/core/workers/dedicated_worker_global_scope.cc

#ifndef MITTO_DOM_DEDICATED_WORKER_GLOBAL_SCOPE_H_
#define MITTO_DOM_DEDICATED_WORKER_GLOBAL_SCOPE_H_

#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "build/build_config.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/worker_global_scope.h"
#include "v8/include/v8-forward.h"

#if BUILDFLAG(IS_WIN)
// base/win/windows_types.h defines PostMessage as PostMessageW, as <windows.h>
// does, which would rename the member wherever it came first. Once its guard
// has seen it, the name stays the member's.
#include "base/win/windows_types.h"
#undef PostMessage
#endif  // BUILDFLAG(IS_WIN)

namespace mitto {

class AnimationFrameController;
class MessagePortChannel;
class SerializedScriptValue;
class WorkerObjectProxy;

class DedicatedWorkerGlobalScope final : public WorkerGlobalScope {
 public:
  // A new scope of a worker named `name`, on the current isolate.
  static DedicatedWorkerGlobalScope* Create(std::u16string name);

  explicit DedicatedWorkerGlobalScope(std::u16string name);
  ~DedicatedWorkerGlobalScope() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  const std::u16string& Name() const { return name_; }
  void PostMessage(v8::Local<v8::Value> message,
                   const std::vector<v8::Local<v8::Object>>& transfer,
                   bindings::ExceptionState& exception_state);
  void PostMessage(v8::Local<v8::Value> message,
                   const bindings::PostMessageOptions& options,
                   bindings::ExceptionState& exception_state);
  // close() is WorkerGlobalScope's.
  // A worker that a window started has frames; one that a worker started has
  // none, and throws.
  int32_t RequestAnimationFrame(v8::Local<v8::Function> callback,
                                bindings::ExceptionState& exception_state);
  void CancelAnimationFrame(int32_t handle);

  // What the Page uses.
  // Where postMessage() and the errors that the worker leaves uncaught go:
  // the Page program's proxy of its Worker, which outlives the scope's
  // context. Without one, as in the Page program given a script, nowhere.
  void SetObjectProxy(WorkerObjectProxy* proxy) { object_proxy_ = proxy; }
  // What runs requestAnimationFrame()'s callbacks: none for a worker that a
  // worker started, or once the worker has ended.
  void SetAnimationFrames(AnimationFrameController* animation_frames) {
    animation_frames_ = animation_frames;
  }
  WorkerObjectProxy* GetObjectProxy() const { return object_proxy_; }
  // A message of its Worker's, with the ports it transfers, from a task of
  // the worker's: a message event, as WorkerGlobalScope::ReceiveMessage()
  // dispatches it.
  void ReceiveMessage(SerializedScriptValue message,
                      std::vector<MessagePortChannel> ports);

 private:
  const std::u16string name_;
  raw_ptr<WorkerObjectProxy> object_proxy_ = nullptr;
  raw_ptr<AnimationFrameController> animation_frames_ = nullptr;
};

}  // namespace mitto

#endif  // MITTO_DOM_DEDICATED_WORKER_GLOBAL_SCOPE_H_
