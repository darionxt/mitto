#include "mitto/dom/navigate_event.h"

#include <stddef.h>

#include <string>
#include <tuple>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_navigate_event.h"
#include "mitto/dom/abort_controller.h"
#include "mitto/dom/abort_signal.h"
#include "mitto/dom/document.h"
#include "mitto/dom/element.h"
#include "mitto/dom/navigation.h"
#include "mitto/dom/navigation_destination.h"
#include "mitto/dom/navigation_precommit_controller.h"
#include "mitto/dom/window.h"
#include "url/origin.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-external.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-promise.h"

namespace mitto {
namespace {

// KURL::ElidedString().
std::string Elided(const std::string& spec) {
  if (spec.size() <= 1024) {
    return spec;
  }
  return base::StrCat({std::string_view(spec).substr(0, 511), "...",
                       std::string_view(spec).substr(spec.size() - 510)});
}

// What a callback that gives a Promise<undefined> gives: a promise that
// adopts what it returned, or one rejected with what it threw, as Web IDL
// converts a callback's result.
v8::Local<v8::Value> Invoke(v8::Isolate* isolate,
                            v8::Local<v8::Function> function,
                            int argc,
                            v8::Local<v8::Value>* argv) {
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::TryCatch try_catch(isolate);
  v8::Local<v8::Value> result;
  if (function->Call(context, v8::Undefined(isolate), argc, argv)
          .ToLocal(&result)) {
    return result;
  }
  v8::Local<v8::Promise::Resolver> rejected;
  if (!try_catch.HasCaught() || !try_catch.CanContinue() ||
      !v8::Promise::Resolver::New(context).ToLocal(&rejected)) {
    return v8::Undefined(isolate);
  }
  std::ignore = rejected->Reject(context, try_catch.Exception());
  return rejected->GetPromise();
}

// Blink's PromiseAll::WaitForAll(): `on_fulfilled` runs once every one of
// `values`, as a promise that adopts it, has fulfilled, and `on_rejected`
// with the reason of the first that is rejected, each from a microtask, also
// for none.
class AllPromises {
 public:
  static void Wait(v8::Isolate* isolate,
                   const std::vector<v8::Local<v8::Value>>& values,
                   base::OnceClosure on_fulfilled,
                   base::OnceCallback<void(v8::Local<v8::Value>)> on_rejected) {
    std::vector<v8::Local<v8::Value>> all = values;
    if (all.empty()) {
      all.push_back(v8::Undefined(isolate));
    }
    auto* const waiter = new AllPromises(all.size(), std::move(on_fulfilled),
                                         std::move(on_rejected));
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();
    const v8::Local<v8::External> data =
        v8::External::New(isolate, waiter, v8::kExternalPointerTypeTagDefault);
    for (const v8::Local<v8::Value> value : all) {
      v8::Local<v8::Promise::Resolver> resolver;
      v8::Local<v8::Function> fulfilled;
      v8::Local<v8::Function> rejected;
      if (!v8::Promise::Resolver::New(context).ToLocal(&resolver) ||
          resolver->Resolve(context, value).IsNothing() ||
          !v8::Function::New(context, &AllPromises::OnFulfilled, data)
               .ToLocal(&fulfilled) ||
          !v8::Function::New(context, &AllPromises::OnRejected, data)
               .ToLocal(&rejected) ||
          resolver->GetPromise()
              ->Then(context, fulfilled, rejected)
              .IsEmpty()) {
        // Only a context that has gone fails these, and nothing settles then.
        waiter->Settled();
      }
    }
  }

 private:
  AllPromises(size_t count,
              base::OnceClosure on_fulfilled,
              base::OnceCallback<void(v8::Local<v8::Value>)> on_rejected)
      : pending_(count),
        outstanding_(count),
        on_fulfilled_(std::move(on_fulfilled)),
        on_rejected_(std::move(on_rejected)) {}

  static AllPromises& From(const v8::FunctionCallbackInfo<v8::Value>& info) {
    return *static_cast<AllPromises*>(info.Data().As<v8::External>()->Value(
        v8::kExternalPointerTypeTagDefault));
  }

  static void OnFulfilled(const v8::FunctionCallbackInfo<v8::Value>& info) {
    AllPromises& waiter = From(info);
    if (--waiter.pending_ == 0 && !waiter.done_) {
      waiter.done_ = true;
      std::move(waiter.on_fulfilled_).Run();
    }
    waiter.Settled();
  }

  static void OnRejected(const v8::FunctionCallbackInfo<v8::Value>& info) {
    AllPromises& waiter = From(info);
    if (!waiter.done_) {
      waiter.done_ = true;
      std::move(waiter.on_rejected_).Run(info[0]);
    }
    waiter.Settled();
  }

  // Goes once every promise has settled. One that never settles keeps the
  // waiter, but not what its callbacks hold weakly, for as long as the
  // process runs.
  void Settled() {
    if (--outstanding_ == 0) {
      delete this;
    }
  }

  size_t pending_;
  size_t outstanding_;
  bool done_ = false;
  base::OnceClosure on_fulfilled_;
  base::OnceCallback<void(v8::Local<v8::Value>)> on_rejected_;
};

}  // namespace

NavigateEvent::Fields::Fields() = default;
NavigateEvent::Fields::Fields(const Fields&) = default;
NavigateEvent::Fields& NavigateEvent::Fields::operator=(const Fields&) =
    default;
NavigateEvent::Fields::~Fields() = default;

// static
NavigateEvent* NavigateEvent::Create(Window* window,
                                     const Fields& fields,
                                     v8::Local<v8::Value> info,
                                     AbortController* controller) {
  NavigateEvent* const event = bindings::MakeGarbageCollected<NavigateEvent>(
      window, fields, info, controller);
  event->SetTrusted(true);
  return event;
}

// static
bindings::ScriptWrappable* NavigateEvent::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  Fields fields;
  fields.navigation_type = u"push";
  fields.cancelable = false;
  return bindings::MakeForKnownGap<NavigateEvent>(
      info, bindings::V8NavigateEvent::kWrapperTypeInfo, nullptr, fields,
      v8::Local<v8::Value>(), AbortController::Create());
}

NavigateEvent::NavigateEvent(Window* window,
                             const Fields& fields,
                             v8::Local<v8::Value> info,
                             AbortController* controller)
    : Event(u"navigate",
            [&fields] {
              bindings::EventInit init;
              init.cancelable = fields.cancelable;
              return init;
            }()),
      window_(window),
      navigation_type_(fields.navigation_type),
      destination_(fields.destination),
      can_intercept_(fields.can_intercept),
      hash_change_(fields.hash_change),
      download_request_(fields.download_request),
      source_element_(fields.source_element),
      url_(fields.url),
      fragment_(fields.fragment),
      controller_(controller) {
  if (!info.IsEmpty()) {
    info_.Reset(v8::Isolate::GetCurrent(), info);
  }
}

NavigateEvent::~NavigateEvent() = default;

const bindings::WrapperTypeInfo* NavigateEvent::GetWrapperTypeInfo() const {
  return &bindings::V8NavigateEvent::kWrapperTypeInfo;
}

void NavigateEvent::Trace(cppgc::Visitor* visitor) const {
  Event::Trace(visitor);
  visitor->Trace(window_);
  visitor->Trace(destination_);
  visitor->Trace(source_element_);
  visitor->Trace(info_);
  visitor->Trace(controller_);
  for (const v8::TracedReference<v8::Function>& handler : handlers_) {
    visitor->Trace(handler);
  }
  for (const v8::TracedReference<v8::Function>& handler : precommit_handlers_) {
    visitor->Trace(handler);
  }
  for (const v8::TracedReference<v8::Value>& result : handler_results_) {
    visitor->Trace(result);
  }
}

AbortSignal* NavigateEvent::Signal() const {
  return controller_->Signal();
}

v8::Local<v8::Value> NavigateEvent::Info() const {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (info_.IsEmpty()) {
    return v8::Undefined(isolate);
  }
  return info_.Get(isolate);
}

bool NavigateEvent::PerformSharedChecks(
    std::string_view function_name,
    bindings::ExceptionState& exception_state) {
  if (!window_ || !window_->HasContext()) {
    exception_state.ThrowDOMException(
        "InvalidStateError",
        base::StrCat(
            {function_name, "() may not be called in a detached window."}));
    return false;
  }
  if (!IsTrusted()) {
    exception_state.ThrowDOMException(
        "SecurityError",
        base::StrCat(
            {function_name, "() may only be called on a trusted event."}));
    return false;
  }
  if (DefaultPrevented()) {
    exception_state.ThrowDOMException(
        "InvalidStateError",
        base::StrCat({function_name,
                      "() may not be called if the event has been canceled."}));
    return false;
  }
  return true;
}

void NavigateEvent::Intercept(
    const bindings::NavigationInterceptOptions& options,
    bindings::ExceptionState& exception_state) {
  if (!PerformSharedChecks("intercept", exception_state)) {
    return;
  }
  if (!can_intercept_) {
    const GURL document_url(window_->GetDocument().UrlForBinding());
    exception_state.ThrowDOMException(
        "SecurityError",
        base::StrCat({"A navigation with URL '", Elided(url_.spec()),
                      "' cannot be intercepted by in a window with origin '",
                      url::Origin::Create(document_url).Serialize(),
                      "' and URL '", Elided(document_url.spec()), "'."}));
    return;
  }
  if (!IsBeingDispatched()) {
    exception_state.ThrowDOMException(
        "InvalidStateError",
        "intercept() may only be called while the navigate event is being "
        "dispatched.");
    return;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (options.precommit_handler) {
    if (!Cancelable()) {
      // Blink's message, which lacks a space.
      exception_state.ThrowDOMException(
          "InvalidStateError",
          "intercept() may only be called with a precommitHandler when the"
          "navigate event is cancelable.");
      return;
    }
    precommit_handlers_.emplace_back(isolate, *options.precommit_handler);
  }
  // A value that another call gave already changes, with the console's
  // warning.
  const auto set_option = [this](std::optional<std::u16string>& kept,
                                 const std::optional<std::u16string>& value,
                                 std::string_view option) {
    if (!value) {
      return;
    }
    if (kept && *kept != *value) {
      window_->AddConsoleMessage(
          ConsoleLevel::kWarning,
          base::StrCat({"The \"", base::UTF16ToUTF8(*value),
                        "\" value for intercept()'s ", option,
                        " will override the previously-passed value of \"",
                        base::UTF16ToUTF8(*kept), "\"."}));
    }
    kept = value;
  };
  set_option(focus_reset_, options.focus_reset, "focusReset option");
  set_option(scroll_behavior_, options.scroll, "scroll option");
  intercept_state_ = InterceptState::kIntercepted;
  if (options.handler) {
    handlers_.emplace_back(isolate, *options.handler);
  }
}

void NavigateEvent::AddHandler(v8::Local<v8::Function> handler,
                               bindings::ExceptionState& exception_state) {
  // NavigationPrecommitController::addHandler(): only before the commit.
  if (intercept_state_ != InterceptState::kIntercepted) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      "navigation has already committed.");
    return;
  }
  handlers_.emplace_back(v8::Isolate::GetCurrent(), handler);
}

void NavigateEvent::Scroll(bindings::ExceptionState& exception_state) {
  if (!PerformSharedChecks("scroll", exception_state)) {
    return;
  }
  const auto refuse = [&exception_state](const char* message) {
    exception_state.ThrowDOMException("InvalidStateError", message);
  };
  switch (intercept_state_) {
    case InterceptState::kFinished:
      refuse("scroll() may not be called after transition completes");
      return;
    case InterceptState::kScrolled:
      refuse("scroll() already called");
      return;
    case InterceptState::kNone:
      refuse("intercept() must be called before scroll()");
      return;
    case InterceptState::kIntercepted:
      refuse("scroll() may not be called before commit.");
      return;
    case InterceptState::kCommitted:
      // ProcessScrollBehavior(): a Page lays nothing out to scroll.
      intercept_state_ = InterceptState::kScrolled;
      return;
  }
}

void NavigateEvent::MaybeCommitImmediately() {
  if (precommit_handlers_.empty()) {
    CommitNow();
    return;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  std::vector<v8::TracedReference<v8::Function>> handlers;
  handlers.swap(precommit_handlers_);
  NavigationPrecommitController* const controller =
      NavigationPrecommitController::Create(this);
  v8::Local<v8::Value> argument = bindings::ToV8(isolate, controller);
  std::vector<v8::Local<v8::Value>> results;
  for (const v8::TracedReference<v8::Function>& handler : handlers) {
    results.push_back(Invoke(isolate, handler.Get(isolate), 1, &argument));
  }
  // Weak, as a promise that never settles would keep the event, and its
  // window, alive: the window's Navigation keeps the event while the
  // navigation is ongoing, which is all the time the event needs.
  const cppgc::WeakPersistent<NavigateEvent> self(this);
  AllPromises::Wait(isolate, results,
                    base::BindOnce(
                        [](const cppgc::WeakPersistent<NavigateEvent>& event) {
                          if (event) {
                            event->CommitNow();
                          }
                        },
                        self),
                    base::BindOnce(
                        [](const cppgc::WeakPersistent<NavigateEvent>& event,
                           v8::Local<v8::Value> reason) {
                          if (event) {
                            event->ReactDone(/*fulfilled=*/false, reason);
                          }
                        },
                        self));
}

void NavigateEvent::CommitNow() {
  if (Signal()->Aborted() || !window_ || !window_->HasContext()) {
    return;
  }
  intercept_state_ = InterceptState::kCommitted;
  Navigation* const navigation = window_->Navigation();
  // popstate for a navigation to a fragment that no navigate() asked for.
  const bool fire_popstate = fragment_ && !navigation->HasOngoingTracker();
  const FrameLoadType type =
      navigation_type_ == u"replace"  ? FrameLoadType::kReplaceCurrentItem
      : navigation_type_ == u"reload" ? FrameLoadType::kReload
                                      : FrameLoadType::kStandard;
  navigation->RunUrlAndHistoryUpdateSteps(url_, type, fire_popstate);
}

void NavigateEvent::FinalizeNavigationActionPromisesList() {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  std::vector<v8::TracedReference<v8::Function>> handlers;
  handlers.swap(handlers_);
  for (const v8::TracedReference<v8::Function>& handler : handlers) {
    handler_results_.emplace_back(
        isolate, Invoke(isolate, handler.Get(isolate), 0, nullptr));
  }
}

void NavigateEvent::React() {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  std::vector<v8::Local<v8::Value>> results;
  for (const v8::TracedReference<v8::Value>& result : handler_results_) {
    results.push_back(result.Get(isolate));
  }
  // Weak, as in MaybeCommitImmediately().
  const cppgc::WeakPersistent<NavigateEvent> self(this);
  AllPromises::Wait(isolate, results,
                    base::BindOnce(
                        [](const cppgc::WeakPersistent<NavigateEvent>& event) {
                          if (event) {
                            event->ReactDone(
                                /*fulfilled=*/true,
                                v8::Undefined(v8::Isolate::GetCurrent()));
                          }
                        },
                        self),
                    base::BindOnce(
                        [](const cppgc::WeakPersistent<NavigateEvent>& event,
                           v8::Local<v8::Value> reason) {
                          if (event) {
                            event->ReactDone(/*fulfilled=*/false, reason);
                          }
                        },
                        self));
}

void NavigateEvent::ReactDone(bool fulfilled, v8::Local<v8::Value> reason) {
  if (!window_ || !window_->HasContext() || Signal()->Aborted()) {
    return;
  }
  Navigation* const navigation = window_->Navigation();
  navigation->ClearOngoingNavigateEvent(this);
  // Focus would be reset and the page scrolled now, which a Page has neither
  // of.
  if (intercept_state_ >= InterceptState::kCommitted) {
    intercept_state_ = InterceptState::kFinished;
  }
  if (fulfilled) {
    navigation->DidFinishOngoingNavigation();
  } else {
    Abort(reason);
  }
}

void NavigateEvent::Abort(v8::Local<v8::Value> reason) {
  if (IsBeingDispatched()) {
    PreventDefault();
  }
  Navigation* const navigation = window_->Navigation();
  NavigateEvent* const previous = navigation->OngoingNavigateEvent();
  controller_->Abort(reason);
  if (navigation->OngoingNavigateEvent() == previous) {
    navigation->ClearOngoingNavigateEvent(previous);
  }
  navigation->DidAbort(reason);
}

}  // namespace mitto
