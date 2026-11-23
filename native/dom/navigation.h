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

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/exception_state.h"
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
struct HistoryItem;
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

  // A navigation that fires a navigate event: one to another document, one
  // within the document to a fragment, pushState() or replaceState(), or a
  // traversal to an entry of the session history.
  struct NavigateParams {
    GURL url;
    bool fragment = false;
    // pushState() and replaceState(), with the state they give.
    bool history_api = false;
    scoped_refptr<const SharedSerializedScriptValue> state_object;
    FrameLoadType load_type = FrameLoadType::kStandard;
    // The entry that a traversal goes to.
    // On the stack only.
    RAW_PTR_EXCLUSION const HistoryItem* destination_item = nullptr;
    // On the stack only, as the garbage collector scans it.
    RAW_PTR_EXCLUSION Element* source_element = nullptr;
  };

  // An entry that the session history gives a new document, as Chrome's
  // browser gives a renderer a NavigationApiHistoryEntry.
  struct ApiEntry {
    ApiEntry();
    ApiEntry(const ApiEntry&);
    ApiEntry& operator=(const ApiEntry&);
    ~ApiEntry();

    std::u16string key;
    std::u16string id;
    std::u16string url;
    int64_t document_sequence = 0;
    scoped_refptr<const SharedSerializedScriptValue> state;
  };
  // What a document's entries come from, as NavigationApiHistoryEntryArrays:
  // the entries of its frame of its origin right before and after its own,
  // earliest first, the entry it came from if it is of its origin, and how
  // the navigation that loaded it went.
  struct EntryArrays {
    EntryArrays();
    EntryArrays(const EntryArrays&);
    EntryArrays& operator=(const EntryArrays&);
    ~EntryArrays();

    std::vector<ApiEntry> back;
    std::vector<ApiEntry> forward;
    std::optional<ApiEntry> previous;
    FrameLoadType type = FrameLoadType::kReplaceCurrentItem;
  };

  // Why the session history did not go to an entry that traverseTo(),
  // back() or forward() asked for: mojom::TraverseCancelledReason.
  enum class TraverseCancelledReason {
    kNotFound,
    kAbortedBeforeCommit,
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
  v8::Local<v8::Object> TraverseTo(const std::u16string& key,
                                   const bindings::NavigationOptions& options);
  v8::Local<v8::Object> Back(const bindings::NavigationOptions& options);
  v8::Local<v8::Object> Forward(const bindings::NavigationOptions& options);
  void UpdateCurrentEntry(
      const bindings::NavigationUpdateCurrentEntryOptions& options,
      bindings::ExceptionState& exception_state);

  // What the Page and the frame loader use.
  //
  // NavigationApi::InitializeForNewWindow(): where the document's entries
  // come from, which the Page gives a document that a navigation loaded,
  // before anything reads them. Its own entry is the window's current one.
  void InitializeForNewWindow(EntryArrays arrays) {
    arrays_ = std::move(arrays);
  }
  // DisposeEntriesForSessionHistoryRemoval(): the session history dropped the
  // entries of `keys`, which go, but the current one, with their dispose
  // events.
  void DisposeEntriesForSessionHistoryRemoval(
      const std::vector<std::u16string>& keys);
  // The session history did not go to the entry of `key`: the traversal that
  // asked for it, if any, ends with its promises rejected.
  void TraverseCancelled(const std::u16string& key,
                         TraverseCancelledReason reason);
  // Fires the navigate event of a navigation, unless the document has no
  // entries, and says whether the navigation goes on, was canceled, or was
  // intercepted, which committed it already, as NavigationApi does.
  DispatchResult DispatchNavigateEvent(const NavigateParams& params);
  // A navigation within the document that gave it the entry `item`, once it
  // has: the entries change, a new one or the current one replaced, or the
  // current one moved for a traversal, the navigate() or traversal that asked
  // for it commits, and currententrychange fires, then the handlers of an
  // intercepted one run.
  void UpdateForNavigation(const HistoryItem& item, FrameLoadType type);
  // DocumentLoader::RunURLAndHistoryUpdateSteps() for an intercepted
  // navigation, as UpdateForSameDocumentNavigation() does it.
  void RunUrlAndHistoryUpdateSteps(
      const GURL& url,
      const HistoryItem* destination_item,
      scoped_refptr<const SharedSerializedScriptValue> state_object,
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
  int64_t DocumentSequence() const;

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

  // A traversal that traverseTo(), back() or forward() asked for, while the
  // session history has yet to go there, by the key of its entry.
  NavigationApiMethodTracker* UpcomingTraverseTracker(
      const std::u16string& key) const;
  // The entry of `key` among the entries, -1 for none.
  int64_t IndexOfKey(const std::u16string& key) const;
  // NavigationApi::MakeEntryFromItem().
  NavigationHistoryEntry* MakeEntryFromItem(const HistoryItem& item);
  // What traverseTo() gives for the current entry: both promises fulfilled
  // with it, as Blink's EarlySuccessResult().
  v8::Local<v8::Object> EarlySuccessResult(NavigationHistoryEntry* entry);
  // What traverseTo() gives once the window may go to the entry of `key`,
  // which is not the current one.
  v8::Local<v8::Object> PerformTraversal(
      const std::u16string& key,
      const bindings::NavigationOptions& options);

  cppgc::Member<Window> window_;
  std::optional<EntryArrays> arrays_;
  bool made_ = false;
  std::vector<cppgc::Member<NavigationHistoryEntry>> entries_;
  int64_t current_index_ = -1;
  cppgc::Member<NavigationActivation> activation_;
  cppgc::Member<NavigationTransition> transition_;
  cppgc::Member<NavigateEvent> ongoing_navigate_event_;
  cppgc::Member<NavigationApiMethodTracker> ongoing_tracker_;
  cppgc::Member<NavigationApiMethodTracker> upcoming_tracker_;
  std::vector<
      std::pair<std::u16string, cppgc::Member<NavigationApiMethodTracker>>>
      upcoming_traverse_trackers_;
  bool has_dropped_navigation_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_NAVIGATION_H_
