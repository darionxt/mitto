// The DOM's AbortSignal, as Blink's: a signal that a controller, a timeout or
// the signals it follows abort, which runs the abort algorithms of the
// features that watch it, then fires `abort`.
//
// A signal that AbortSignal.any() makes, a composite one, follows signals
// that abort only from outside, its sources, as Blink's composition managers
// link them: those it is given, and the sources of a composite one among them.
// A source holds its dependents weakly, as Blink's does, so that a composite
// signal that nothing reaches goes. Blink's AbortSignalRegistry keeps one that
// has `abort` listeners while it can still abort; here its sources keep it
// then, which keeps it as long as one of them can abort it.
//
// Upstream: third_party/blink/renderer/core/dom/abort_signal.h
// Upstream: third_party/blink/renderer/core/dom/abort_signal.cc
// Upstream: third_party/blink/renderer/core/dom/abort_signal_composition_manager.cc
// Upstream: third_party/blink/renderer/core/dom/abort_signal_registry.cc

#ifndef MITTO_DOM_ABORT_SIGNAL_H_
#define MITTO_DOM_ABORT_SIGNAL_H_

#include <stdint.h>

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "base/time/time.h"
#include "mitto/dom/event_target.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-traced-handle.h"
#include "v8/include/v8-value.h"

namespace base {
class DeadlineTimer;
}  // namespace base

namespace v8 {
class Context;
}  // namespace v8

namespace mitto {

class AbortSignal : public EventTarget {
 public:
  enum class SignalType {
    // An AbortController's.
    kController,
    // One that AbortSignal.abort() made aborted.
    kAborted,
    // One that AbortSignal.timeout() made.
    kTimeout,
    // One that follows others, as AbortSignal.any() makes.
    kComposite,
  };

  // An abort algorithm: what a feature does when the signal aborts, before
  // `abort` fires. It runs at most once.
  class Algorithm : public cppgc::GarbageCollected<Algorithm> {
   public:
    Algorithm();
    virtual ~Algorithm();
    virtual void Run() = 0;
    virtual void Trace(cppgc::Visitor* visitor) const;

   private:
    friend class AbortSignal;
    // The next algorithm of the signal, in the order they were added.
    cppgc::Member<Algorithm> next_;
  };

  // A composite signal that follows nothing, and so never aborts, as a Known
  // Gap gives one.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);
  // A composite signal that follows `sources`, as a Request's signal does.
  static AbortSignal* CreateComposite(const std::vector<AbortSignal*>& sources);

  explicit AbortSignal(SignalType type);
  ~AbortSignal() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  static AbortSignal* Abort(std::optional<v8::Local<v8::Value>> reason);
  static AbortSignal* Timeout(uint64_t milliseconds);
  static AbortSignal* Any(const std::vector<AbortSignal*>& signals);
  bool Aborted() const { return !abort_reason_.IsEmpty(); }
  v8::Local<v8::Value> Reason() const;
  void ThrowIfAborted() const;

  // The DOM standard's "add an algorithm", which does nothing once the signal
  // has aborted or can no longer abort.
  void AddAlgorithm(Algorithm* algorithm);
  // "Signal abort", for a signal that is not composite: with no reason given,
  // an AbortError, as AbortController.abort() gives.
  void SignalAbort(std::optional<v8::Local<v8::Value>> reason);

 protected:
  void ListenersChanged(std::u16string_view type) override;

  // For a composite signal: follows the sources of `signals` that can still
  // abort it, or aborts at once with the reason of the first of them that has
  // aborted. With no source to follow, it can never abort.
  void FollowSignals(const std::vector<AbortSignal*>& signals);

 private:
  // A dependent in its source's list, held weakly, and strongly while the
  // dependent has `abort` listeners.
  struct DependentLink;
  // A source in its dependent's list.
  struct SourceLink;

  bool IsComposite() const { return type_ == SignalType::kComposite; }
  void AddSource(AbortSignal* source);
  // Sets the reason, an AbortError for undefined.
  void SetAbortReason(v8::Local<v8::Value> reason);
  // Runs the abort algorithms in order, then fires `abort`.
  void RunAbortSteps();
  // The signal can no longer abort, or has aborted: it lets go of its
  // algorithms and its links, and tells its dependents.
  void Settle();
  // `source` can no longer abort this composite signal.
  void SourceSettled(AbortSignal* source);
  // Has this composite signal's sources hold it strongly while it has `abort`
  // listeners and can still abort.
  void UpdateKeptBySources();
  // Aborts a timeout signal after `delay`, in the current context, from a
  // timer of the Page's event loop, as setTimeout() runs its timers, unless the
  // context goes first.
  void StartTimeout(base::TimeDelta delay);
  void TimeoutFired();
  // The context went: the timer stops and lets the signal go, as Blink's
  // ContextDestroyed() cancels the task.
  void CancelTimeout();

  const SignalType type_;
  v8::TracedReference<v8::Value> abort_reason_;
  bool settled_ = false;
  bool running_abort_steps_ = false;
  // The abort algorithms, and a source's dependents and a composite signal's
  // sources, each as a list of objects on the C++ heap, which cppgc can trace
  // while scripts run.
  cppgc::Member<Algorithm> first_algorithm_;
  cppgc::Member<Algorithm> last_algorithm_;
  cppgc::Member<DependentLink> first_dependent_;
  cppgc::Member<DependentLink> last_dependent_;
  cppgc::Member<SourceLink> first_source_;
  // A timeout signal's timer, its context, and the signal itself, which the
  // timer keeps until it fires, as Blink's posted task holds the signal.
  std::unique_ptr<base::DeadlineTimer> timeout_timer_;
  v8::Global<v8::Context> timeout_context_;
  cppgc::Persistent<AbortSignal> kept_by_timeout_;
  // The ID of the window's callback that cancels the timer.
  uint64_t timeout_cancel_id_ = 0;
};

}  // namespace mitto

#endif  // MITTO_DOM_ABORT_SIGNAL_H_
