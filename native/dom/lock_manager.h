// navigator.locks: the Web Locks API of a window, as Blink's LockManager runs
// it over the Page's LockService. A request checks its options, waits for the
// first answer about storage access, which the browser gives from a task of
// its own and the manager keeps, then asks the service, and calls its callback
// once the lock is granted, or with null once a request that would not wait
// fails. Its promise settles as the callback's result does, once the lock is
// let go. A request aborted by its signal before it is granted rejects with the
// signal's reason. When the window's context goes, its requests and locks go.
//
// Upstream: third_party/blink/renderer/modules/locks/lock_manager.h
// Upstream: third_party/blink/renderer/modules/locks/lock_manager.cc

#ifndef MITTO_DOM_LOCK_MANAGER_H_
#define MITTO_DOM_LOCK_MANAGER_H_

#include <stdint.h>

#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/heap_vector.h"
#include "mitto/dom/lock_service.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-promise.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class AbortSignal;
class Lock;
class Window;

class LockManager final : public bindings::ScriptWrappable {
 public:
  // The manager of `window`, whose context it lives in.
  explicit LockManager(Window& window);
  ~LockManager() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  v8::Local<v8::Promise> Request(const std::u16string& name,
                                 v8::Local<v8::Function> callback,
                                 bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Request(const std::u16string& name,
                                 const bindings::LockOptions& options,
                                 v8::Local<v8::Function> callback,
                                 bindings::ExceptionState& exception_state);
  v8::Local<v8::Promise> Query(bindings::ExceptionState& exception_state);

  // `lock` no longer holds its lock.
  void OnLockReleased(Lock* lock);

 private:
  class Connection;
  class LockRequest;
  class AbortRequest;

  // Whether the document may reach Web Locks: not for an opaque origin, nor
  // once the window's context has gone, for which this throws.
  bool CheckAccess(bindings::ExceptionState& exception_state) const;
  // Runs `callback` once the browser has said that the document may use
  // storage, which it answers from a task of its own the first time. A Page's
  // documents always may, so the SecurityError that a refusal gives never
  // comes.
  void CheckStorageAccessAllowed(base::OnceClosure callback);
  void RequestImpl(LockRequest* request,
                   LockService::WaitMode wait,
                   AbortSignal* signal);
  // What the service tells the requests, from the tasks the connection runs.
  void Granted(int64_t lock_id);
  void Failed(int64_t lock_id);
  void Broken(int64_t lock_id);
  LockRequest* TakePendingRequest(int64_t lock_id);
  void ContextDestroyed();

  // The document's origin, whose lock group this asks, and the client ID of
  // the document.
  const std::string origin_;
  const std::string client_id_;
  const scoped_refptr<Connection> connection_;
  HeapVector<LockRequest> pending_requests_;
  HeapVector<Lock> held_locks_;
  std::optional<bool> cached_allowed_;
  bool context_destroyed_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_LOCK_MANAGER_H_
