// The HTML Standard's Navigation, the window's navigation, as Blink's
// NavigationApi has it: the document's entries of its tab's session history,
// the ones before it of its origin that the Page carries from the document
// before it and its own, the navigate event that each navigation its document
// starts fires, navigate() and reload(), and what a navigation within the
// document does to the entries. A document of an opaque origin, such as
// about:blank, and the initial about:blank have no entries and fire no
// events, as Blink's HasEntriesAndEventsDisabled() says. Traversal and
// updateCurrentEntry() are Known Gaps.
//
// Upstream: third_party/blink/renderer/core/navigation_api/navigation_api.cc
// Upstream: third_party/blink/renderer/core/navigation_api/navigation_api_method_tracker.cc
// Upstream: third_party/blink/renderer/core/frame/history_util.cc

#ifndef MITTO_DOM_NAVIGATION_H_
#define MITTO_DOM_NAVIGATION_H_

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/event_target.h"
#include "mitto/dom/frame_loader.h"
#include "mitto/dom/serialized_script_value.h"
#include "url/gurl.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-value.h"

namespace mitto {

class Element;
class NavigateEvent;
class NavigationActivation;
class NavigationApiMethodTracker;
class NavigationHistoryEntry;
class NavigationTransition;
class Window;

// Blink's CanChangeToUrlForHistoryApi(): whether a history API may give a
// document at `document_url` the URL `url` with no navigation to another
// document: the same scheme, credentials, host and port, and for a URL that is
// not standard or a document of an opaque origin, the same path.
bool CanChangeToUrlForHistoryApi(const GURL& url, const GURL& document_url);

class Navigation final : public EventTarget {
 public:
  // What DispatchNavigateEvent() tells the navigation that fired it.
  enum class DispatchResult {
    kContinue,
    kAbort,
    kIntercept,
  };

  // A navigation that fires a navigate event: one to another document, or
  // one within the document to a fragment.
  struct NavigateParams {
    GURL url;
    bool fragment = false;
    FrameLoadType load_type = FrameLoadType::kStandard;
    // On the stack only, as the garbage collector scans it.
    RAW_PTR_EXCLUSION Element* source_element = nullptr;
  };

  // An entry as the Page carries it from one document to the next.
  struct CarriedEntry {
    CarriedEntry();
    CarriedEntry(const CarriedEntry&);
    CarriedEntry& operator=(const CarriedEntry&);
    ~CarriedEntry();

    std::u16string key;
    std::u16string id;
    std::u16string url;
    std::optional<SerializedScriptValue> state;
  };
  // What a document's entries come from: the entries of the document before
  // it, with the index of its own, and how the navigation that loaded this
  // one went, "push", "replace" or "reload".
  struct Carried {
    Carried();
    Carried(const Carried&);
    Carried& operator=(const Carried&);
    ~Carried();

    std::vector<CarriedEntry> entries;
    size_t current = 0;
    std::u16string url;
    std::u16string navigation_type = u"replace";
  };

  static Navigation* Create(Window* window);
  // One of no window, with no entries, for a Known Gap that gives one.
  static bindings::ScriptWrappable* CreateForKnownGap(
      const bindings::WrapperTypeInfo& info);

  explicit Navigation(Window* window);
  ~Navigation() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  NavigationHistoryEntry* CurrentEntry();
  NavigationTransition* Transition() const { return transition_.Get(); }
  NavigationActivation* Activation();
  bool CanGoBack();
  bool CanGoForward();
  std::vector<NavigationHistoryEntry*> Entries();
  v8::Local<v8::Object> Navigate(
      const std::u16string& url,
      const bindings::NavigationNavigateOptions& options);
  v8::Local<v8::Object> Reload(
      const bindings::NavigationReloadOptions& options);

  // What the Page and the frame loader use.
  //
  // Where the document's entries come from, which the Page gives a document
  // that a navigation loaded, before anything reads them.
  void SetCarried(Carried carried) { carried_ = std::move(carried); }
  // The document's entries, for the document after it.
  Carried CarryForNextDocument();
  // Fires the navigate event of a navigation, unless the document has no
  // entries, and says whether the navigation goes on, was canceled, or was
  // intercepted, which committed it already, as NavigationApi does.
  DispatchResult DispatchNavigateEvent(const NavigateParams& params);
  // A navigation within the document that gave it `url`, once it has: the
  // entries change, the navigate() that asked for it commits, and
  // currententrychange fires, then the handlers of an intercepted one run.
  // A new entry keeps the current one's state if `keep_state`, as one to a
  // fragment does.
  void UpdateForNavigation(const GURL& url,
                           FrameLoadType type,
                           bool keep_state);
  // DocumentLoader::RunURLAndHistoryUpdateSteps() for an intercepted
  // navigation: the document takes `url`, the Session hears of it, the
  // entries change, and popstate fires if `fire_popstate`.
  void RunUrlAndHistoryUpdateSteps(const GURL& url,
                                   FrameLoadType type,
                                   bool fire_popstate);
  // The ongoing navigation's end: it went well, or `reason` aborted it.
  void DidFinishOngoingNavigation();
  void DidAbort(v8::Local<v8::Value> reason);
  // Aborts the ongoing navigation with Blink's AbortError.
  void AbortOngoingNavigation();
  NavigateEvent* OngoingNavigateEvent() const {
    return ongoing_navigate_event_.Get();
  }
  void ClearOngoingNavigateEvent(NavigateEvent* event);
  bool HasOngoingTracker() const;
  int64_t GetIndexFor(const NavigationHistoryEntry* entry) const;
  // The document's sequence number, which an entry of it shares.
  uint64_t DocumentSequence() const { return document_sequence_; }

 private:
  bool HasEntriesAndEventsDisabled() const;
  // Makes the entries and the activation the first time anything needs them,
  // for a document that has them.
  void MakeEntries();
  // What navigate() and reload() give for an error before they navigate:
  // both promises rejected with `error`, as Blink's EarlyErrorResult().
  v8::Local<v8::Object> EarlyErrorResult(v8::Local<v8::Value> error);
  // NavigationApi::PerformSharedNavigationChecks().
  bool PerformSharedNavigationChecks(std::string_view method,
                                     v8::Local<v8::Value>* error);
  // Starts a navigation that navigate() or reload() asked for, which its
  // tracker hears of, and returns the tracker's promises.
  v8::Local<v8::Object> NavigateWithTracker(NavigationApiMethodTracker* tracker,
                                            const FrameLoadRequest& request,
                                            FrameLoadType type);

  cppgc::Member<Window> window_;
  std::optional<Carried> carried_;
  bool made_ = false;
  uint64_t document_sequence_ = 0;
  std::vector<cppgc::Member<NavigationHistoryEntry>> entries_;
  int64_t current_index_ = -1;
  cppgc::Member<NavigationActivation> activation_;
  cppgc::Member<NavigationTransition> transition_;
  cppgc::Member<NavigateEvent> ongoing_navigate_event_;
  cppgc::Member<NavigationApiMethodTracker> ongoing_tracker_;
  cppgc::Member<NavigationApiMethodTracker> upcoming_tracker_;
  bool has_dropped_navigation_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_NAVIGATION_H_
