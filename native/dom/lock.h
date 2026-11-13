// A Web Lock that a request holds: its name and mode, and the hold that lasts
// until the promise that the request's callback gave settles, which then
// settles the request's promise the same way. A request that steals the lock
// breaks the hold, and the request's promise rejects.
//
// Upstream: third_party/blink/renderer/modules/locks/lock.h
// Upstream: third_party/blink/renderer/modules/locks/lock.cc

#ifndef MITTO_DOM_LOCK_H_
#define MITTO_DOM_LOCK_H_

#include <stdint.h>

#include <string>

#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/lock_service.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-traced-handle.h"

namespace mitto {

class LockManager;

class Lock final : public bindings::ScriptWrappable {
 public:
  Lock(std::u16string name,
       LockService::Mode mode,
       int64_t lock_id,
       LockManager* manager);
  ~Lock() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  const std::u16string& Name() const { return name_; }
  std::u16string Mode() const;

  int64_t lock_id() const { return lock_id_; }

  // Holds the lock until `promise` settles, then settles `resolver` as it
  // did.
  void HoldUntil(v8::Local<v8::Context> context,
                 v8::Local<v8::Promise> promise,
                 v8::Local<v8::Promise::Resolver> resolver);
  // Lets the lock go, if it still holds it.
  void ReleaseIfHeld();
  // A request that stole the lock broke this hold.
  void OnBroken(v8::Local<v8::Context> context);

  // LockMode's value for `mode`.
  static std::u16string ModeName(LockService::Mode mode);

 private:
  // What the hold's promise settled with, for `fulfilled` or not.
  static void Settled(const v8::FunctionCallbackInfo<v8::Value>& info,
                      bool fulfilled);

  const std::u16string name_;
  const LockService::Mode mode_;
  const int64_t lock_id_;
  bool held_ = true;
  cppgc::Member<LockManager> manager_;
  v8::TracedReference<v8::Promise::Resolver> resolver_;
};

}  // namespace mitto

#endif  // MITTO_DOM_LOCK_H_
