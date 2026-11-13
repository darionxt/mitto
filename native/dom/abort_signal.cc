#include "mitto/dom/abort_signal.h"

#include <memory>
#include <string_view>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/exception_context.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/v8_abort_signal.h"
#include "mitto/dom/event.h"
#include "mitto/dom/window.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

namespace {

// A DOMException with `name` and `message`, with the stack where it is made,
// as Blink's V8ThrowDOMException::CreateOrEmpty() makes one.
v8::Local<v8::Value> NewDOMException(v8::Isolate* isolate,
                                     std::string_view name,
                                     std::string_view message) {
  return bindings::PerIsolateData::From(isolate)->dom_exception_hooks().create(
      isolate, name, message);
}

}  // namespace

struct AbortSignal::DependentLink final
    : public cppgc::GarbageCollected<DependentLink> {
  explicit DependentLink(AbortSignal* signal) : dependent(signal) {}

  void Trace(cppgc::Visitor* visitor) const {
    visitor->Trace(dependent);
    visitor->Trace(kept);
    visitor->Trace(next);
  }

  cppgc::WeakMember<AbortSignal> dependent;
  // The dependent, while it has `abort` listeners and can still abort.
  cppgc::Member<AbortSignal> kept;
  cppgc::Member<DependentLink> next;
};

struct AbortSignal::SourceLink final
    : public cppgc::GarbageCollected<SourceLink> {
  SourceLink(AbortSignal* signal, DependentLink* dependent_link)
      : source(signal), link(dependent_link) {}

  void Trace(cppgc::Visitor* visitor) const {
    visitor->Trace(source);
    visitor->Trace(link);
    visitor->Trace(next);
  }

  cppgc::WeakMember<AbortSignal> source;
  // The dependent's link in the source's list.
  cppgc::WeakMember<DependentLink> link;
  cppgc::Member<SourceLink> next;
};

AbortSignal::Algorithm::Algorithm() = default;
AbortSignal::Algorithm::~Algorithm() = default;

void AbortSignal::Algorithm::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(next_);
}

// static
bindings::ScriptWrappable* AbortSignal::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  bindings::ScriptWrappable* const signal =
      bindings::MakeForKnownGap<AbortSignal>(
          info, bindings::V8AbortSignal::kWrapperTypeInfo,
          SignalType::kComposite);
  static_cast<AbortSignal*>(signal)->FollowSignals({});
  return signal;
}

// static
AbortSignal* AbortSignal::CreateComposite(
    const std::vector<AbortSignal*>& sources) {
  AbortSignal* const signal =
      bindings::MakeGarbageCollected<AbortSignal>(SignalType::kComposite);
  signal->FollowSignals(sources);
  return signal;
}

AbortSignal::AbortSignal(SignalType type) : type_(type) {}

AbortSignal::~AbortSignal() = default;

const bindings::WrapperTypeInfo* AbortSignal::GetWrapperTypeInfo() const {
  return &bindings::V8AbortSignal::kWrapperTypeInfo;
}

void AbortSignal::Trace(cppgc::Visitor* visitor) const {
  EventTarget::Trace(visitor);
  visitor->Trace(abort_reason_);
  visitor->Trace(first_algorithm_);
  visitor->Trace(last_algorithm_);
  visitor->Trace(first_dependent_);
  visitor->Trace(last_dependent_);
  visitor->Trace(first_source_);
}

// static
AbortSignal* AbortSignal::Abort(std::optional<v8::Local<v8::Value>> reason) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  AbortSignal* const signal =
      bindings::MakeGarbageCollected<AbortSignal>(SignalType::kAborted);
  signal->abort_reason_.Reset(
      isolate, reason ? *reason
                      : NewDOMException(isolate, "AbortError",
                                        "signal is aborted without reason"));
  signal->Settle();
  return signal;
}

// static
AbortSignal* AbortSignal::Timeout(uint64_t milliseconds) {
  AbortSignal* const signal =
      bindings::MakeGarbageCollected<AbortSignal>(SignalType::kTimeout);
  // A context that is gone runs no task.
  if (!Window::Current().HasContext()) {
    return signal;
  }
  signal->StartTimeout(base::Milliseconds(milliseconds));
  return signal;
}

// static
AbortSignal* AbortSignal::Any(const std::vector<AbortSignal*>& signals) {
  return CreateComposite(signals);
}

v8::Local<v8::Value> AbortSignal::Reason() const {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (abort_reason_.IsEmpty()) {
    return v8::Undefined(isolate);
  }
  return abort_reason_.Get(isolate);
}

void AbortSignal::ThrowIfAborted() const {
  if (!Aborted()) {
    return;
  }
  // The reason leaves as it is, with no call's context in front of a message
  // of its own.
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const bindings::OmitExceptionContextScope omit(isolate);
  isolate->ThrowException(abort_reason_.Get(isolate));
}

void AbortSignal::AddAlgorithm(Algorithm* algorithm) {
  if (Aborted() || settled_) {
    return;
  }
  CHECK(!running_abort_steps_);
  if (last_algorithm_) {
    last_algorithm_->next_ = algorithm;
  } else {
    first_algorithm_ = algorithm;
  }
  last_algorithm_ = algorithm;
}

void AbortSignal::SignalAbort(std::optional<v8::Local<v8::Value>> reason) {
  CHECK(!IsComposite());
  if (Aborted()) {
    return;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  SetAbortReason(reason ? *reason
                        : NewDOMException(isolate, "AbortError",
                                          "signal is aborted without reason"));
  // Every dependent has its reason before any script runs, in an algorithm or
  // a listener.
  std::vector<cppgc::Persistent<AbortSignal>> dependents_to_abort;
  for (DependentLink* link = first_dependent_.Get(); link; link = link->next) {
    AbortSignal* const dependent = link->dependent.Get();
    if (dependent && !dependent->Aborted()) {
      dependent->SetAbortReason(Reason());
      dependents_to_abort.emplace_back(dependent);
    }
  }
  RunAbortSteps();
  for (const cppgc::Persistent<AbortSignal>& dependent : dependents_to_abort) {
    dependent->RunAbortSteps();
    dependent->Settle();
  }
  Settle();
}

void AbortSignal::ListenersChanged(std::u16string_view type) {
  if (type == u"abort" && IsComposite()) {
    UpdateKeptBySources();
  }
}

void AbortSignal::FollowSignals(const std::vector<AbortSignal*>& signals) {
  CHECK(IsComposite());
  for (AbortSignal* const signal : signals) {
    CHECK(signal);
    if (signal->Aborted()) {
      abort_reason_.Reset(v8::Isolate::GetCurrent(), signal->Reason());
      Settle();
      return;
    }
  }
  for (AbortSignal* const signal : signals) {
    if (!signal->IsComposite()) {
      AddSource(signal);
      continue;
    }
    for (SourceLink* link = signal->first_source_.Get(); link;
         link = link->next) {
      if (AbortSignal* const source = link->source.Get()) {
        AddSource(source);
      }
    }
  }
  if (!first_source_) {
    Settle();
  }
}

void AbortSignal::AddSource(AbortSignal* source) {
  DCHECK(!source->IsComposite());
  // A source that can no longer abort needs no following.
  if (source->settled_) {
    return;
  }
  // The same signal given twice, as in AbortSignal.any([signal, signal]).
  SourceLink* last = nullptr;
  for (SourceLink* link = first_source_.Get(); link; link = link->next) {
    if (link->source.Get() == source) {
      return;
    }
    last = link;
  }
  // The source's list loses the dependents that have gone.
  DependentLink* previous = nullptr;
  for (DependentLink* link = source->first_dependent_.Get(); link;
       link = link->next) {
    if (link->dependent) {
      previous = link;
      continue;
    }
    if (previous) {
      previous->next = link->next;
    } else {
      source->first_dependent_ = link->next;
    }
  }
  source->last_dependent_ = previous;
  DependentLink* const dependent_link =
      bindings::MakeGarbageCollected<DependentLink>(this);
  if (source->last_dependent_) {
    source->last_dependent_->next = dependent_link;
  } else {
    source->first_dependent_ = dependent_link;
  }
  source->last_dependent_ = dependent_link;
  SourceLink* const source_link =
      bindings::MakeGarbageCollected<SourceLink>(source, dependent_link);
  if (last) {
    last->next = source_link;
  } else {
    first_source_ = source_link;
  }
}

void AbortSignal::SetAbortReason(v8::Local<v8::Value> reason) {
  CHECK(!Aborted());
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (reason->IsUndefined()) {
    abort_reason_.Reset(
        isolate, NewDOMException(isolate, "AbortError",
                                 "signal is aborted with undefined reason"));
    return;
  }
  abort_reason_.Reset(isolate, reason);
}

void AbortSignal::RunAbortSteps() {
  CHECK(!running_abort_steps_);
  running_abort_steps_ = true;
  std::vector<cppgc::Persistent<Algorithm>> algorithms;
  for (Algorithm* algorithm = first_algorithm_.Get(); algorithm;
       algorithm = algorithm->next_) {
    algorithms.emplace_back(algorithm);
  }
  for (const cppgc::Persistent<Algorithm>& algorithm : algorithms) {
    algorithm->Run();
  }
  running_abort_steps_ = false;
  const bindings::EventInit init;
  Event* const event = Event::Create(u"abort", init);
  event->SetTrusted(true);
  DispatchEvent(event);
}

void AbortSignal::Settle() {
  if (settled_) {
    return;
  }
  settled_ = true;
  first_algorithm_ = nullptr;
  last_algorithm_ = nullptr;
  // A composite signal's sources no longer keep it.
  for (SourceLink* link = first_source_.Get(); link; link = link->next) {
    if (DependentLink* const dependent_link = link->link.Get()) {
      dependent_link->kept = nullptr;
    }
  }
  first_source_ = nullptr;
  // A source's dependents lose it.
  std::vector<cppgc::Persistent<AbortSignal>> dependents;
  for (DependentLink* link = first_dependent_.Get(); link; link = link->next) {
    if (AbortSignal* const dependent = link->dependent.Get()) {
      dependents.emplace_back(dependent);
    }
  }
  first_dependent_ = nullptr;
  last_dependent_ = nullptr;
  for (const cppgc::Persistent<AbortSignal>& dependent : dependents) {
    if (!dependent->settled_) {
      dependent->SourceSettled(this);
    }
  }
}

void AbortSignal::SourceSettled(AbortSignal* source) {
  DCHECK(IsComposite());
  SourceLink* previous = nullptr;
  for (SourceLink* link = first_source_.Get(); link; link = link->next) {
    if (link->source.Get() != source) {
      previous = link;
      continue;
    }
    if (previous) {
      previous->next = link->next;
    } else {
      first_source_ = link->next;
    }
    break;
  }
  if (!first_source_) {
    Settle();
  }
}

void AbortSignal::UpdateKeptBySources() {
  if (settled_) {
    return;
  }
  AbortSignal* const kept = HasEventListeners(u"abort") ? this : nullptr;
  for (SourceLink* link = first_source_.Get(); link; link = link->next) {
    if (DependentLink* const dependent_link = link->link.Get()) {
      dependent_link->kept = kept;
    }
  }
}

void AbortSignal::StartTimeout(base::TimeDelta delay) {
  const base::TimeTicks deadline = base::TimeTicks::Now() + delay;
  // A deadline beyond any time never comes.
  if (deadline.is_max()) {
    return;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  timeout_context_.Reset(isolate, isolate->GetCurrentContext());
  kept_by_timeout_ = this;
  timeout_timer_ = std::make_unique<base::DeadlineTimer>();
  // Blink posts the task with base's default policy, which may run it late.
  timeout_timer_->Start(
      FROM_HERE, deadline,
      base::BindOnce(&AbortSignal::TimeoutFired, base::Unretained(this)),
      base::subtle::DelayPolicy::kFlexibleNoSooner);
}

void AbortSignal::TimeoutFired() {
  // Let go of the signal once this returns.
  const cppgc::Persistent<AbortSignal> self = std::move(kept_by_timeout_);
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::HandleScope handle_scope(isolate);
  const v8::Local<v8::Context> context = timeout_context_.Get(isolate);
  timeout_context_.Reset();
  const v8::Context::Scope context_scope(context);
  // A window whose document has gone runs nothing more.
  if (!Window::Current().HasContext()) {
    return;
  }
  SignalAbort(NewDOMException(isolate, "TimeoutError", "signal timed out"));
}

}  // namespace mitto
