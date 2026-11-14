// The HTML Standard's NavigateEvent, which the window's navigation fires as a
// navigation that its document starts begins, as Blink's NavigationApi
// dispatches it: canceling it stops the navigation, and intercept() makes one
// to a URL that a history API may give the document a navigation within it,
// which commits at once and ends once its handlers' promises settle.
//
// Upstream: third_party/blink/renderer/core/navigation_api/navigate_event.cc

#ifndef MITTO_DOM_NAVIGATE_EVENT_H_
#define MITTO_DOM_NAVIGATE_EVENT_H_

#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/event.h"
#include "mitto/dom/frame_loader.h"
#include "url/gurl.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-traced-handle.h"
#include "v8/include/v8-value.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class AbortController;
class AbortSignal;
class Element;
class NavigationDestination;
class Window;

class NavigateEvent final : public Event {
 public:
  // What NavigationApi::DispatchNavigateEvent() gives an event.
  struct Fields {
    Fields();
    Fields(const Fields&);
    Fields& operator=(const Fields&);
    ~Fields();

    // On the stack only, as the garbage collector scans it.
    std::u16string navigation_type;
    RAW_PTR_EXCLUSION NavigationDestination* destination = nullptr;
    bool can_intercept = false;
    bool hash_change = false;
    bool cancelable = true;
    std::optional<std::u16string> download_request;
    RAW_PTR_EXCLUSION Element* source_element = nullptr;
    // Where it goes, and whether it is to a fragment, as Blink's dispatch
    // params keep them.
    GURL url;
    bool fragment = false;
  };

  // A trusted navigate event of `window`, whose signal `controller` aborts.
  static NavigateEvent* Create(Window* window,
                               const Fields& fields,
                               v8::Local<v8::Value> info,
                               AbortController* controller);
  // One of no window, for a Known Gap that gives one, as a page's own
  // constructor does.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  NavigateEvent(Window* window,
                const Fields& fields,
                v8::Local<v8::Value> info,
                AbortController* controller);
  ~NavigateEvent() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  const std::u16string& NavigationType() const { return navigation_type_; }
  NavigationDestination* Destination() const { return destination_.Get(); }
  bool CanIntercept() const { return can_intercept_; }
  // No user activates a Page.
  bool UserInitiated() const { return false; }
  bool HashChange() const { return hash_change_; }
  AbortSignal* Signal() const;
  // A form's data, which only a form's navigation has: #281's.
  bindings::ScriptWrappable* FormData() const { return nullptr; }
  const std::optional<std::u16string>& DownloadRequest() const {
    return download_request_;
  }
  v8::Local<v8::Value> Info() const;
  bool HasUAVisualTransition() const { return false; }
  Element* SourceElement() const { return source_element_.Get(); }
  void Intercept(const bindings::NavigationInterceptOptions& options,
                 bindings::ExceptionState& exception_state);
  void Scroll(bindings::ExceptionState& exception_state);

  // What the window's Navigation uses.
  //
  // Whether intercept() was called.
  bool HasNavigationActions() const {
    return intercept_state_ != InterceptState::kNone;
  }
  // Commits an intercepted navigation, once its precommit handlers' promises
  // have fulfilled, at once for none.
  void MaybeCommitImmediately();
  // Calls the handlers that intercept() was given, once the navigation has
  // committed, and keeps the promises they give.
  void FinalizeNavigationActionPromisesList();
  // Waits for those promises: the navigation ends well once all have
  // fulfilled, and is aborted at the first that is rejected.
  void React();
  // Aborts the navigation with `reason`: its signal aborts, and the window's
  // Navigation says so.
  void Abort(v8::Local<v8::Value> reason);
  // NavigationPrecommitController::addHandler().
  void AddHandler(v8::Local<v8::Function> handler,
                  bindings::ExceptionState& exception_state);
  // What the navigation's handlers and promises are kept until.
  Window* GetWindow() const { return window_.Get(); }

 private:
  enum class InterceptState {
    kNone,
    kIntercepted,
    kCommitted,
    kScrolled,
    kFinished,
  };

  // What intercept() and scroll() check first, Blink's PerformSharedChecks().
  bool PerformSharedChecks(std::string_view function_name,
                           bindings::ExceptionState& exception_state);
  void CommitNow();
  void ReactDone(bool fulfilled, v8::Local<v8::Value> reason);

  cppgc::Member<Window> window_;
  const std::u16string navigation_type_;
  cppgc::Member<NavigationDestination> destination_;
  const bool can_intercept_;
  const bool hash_change_;
  const std::optional<std::u16string> download_request_;
  cppgc::Member<Element> source_element_;
  const GURL url_;
  const bool fragment_;
  v8::TracedReference<v8::Value> info_;
  cppgc::Member<AbortController> controller_;
  InterceptState intercept_state_ = InterceptState::kNone;
  std::optional<std::u16string> focus_reset_;
  std::optional<std::u16string> scroll_behavior_;
  std::vector<v8::TracedReference<v8::Function>> handlers_;
  std::vector<v8::TracedReference<v8::Function>> precommit_handlers_;
  std::vector<v8::TracedReference<v8::Value>> handler_results_;
};

}  // namespace mitto

#endif  // MITTO_DOM_NAVIGATE_EVENT_H_
