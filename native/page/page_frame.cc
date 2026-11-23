#include "mitto/page/page_frame.h"

#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "mitto/dom/document.h"
#include "mitto/dom/document_load_timing.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/history_item.h"
#include "mitto/dom/navigation.h"
#include "mitto/dom/window.h"
#include "mitto/page/document_loader.h"
#include "mitto/page/page_fetcher.h"
#include "mitto/page/page_requests.h"
#include "mitto/page/script_runner.h"
#include "mitto/page/session_cookie_jar.h"
#include "mitto/page/session_history.h"
#include "url/origin.h"

namespace mitto {
namespace {

// RenderFrameImpl's window for a navigation the same as the one that waits,
// which it drops: kNavigationDuplicateDetectionTimeWindow.
constexpr base::TimeDelta kDuplicateWindow = base::Milliseconds(3000);

}  // namespace

PageFrame::PageFrame(ScriptRunner& runner,
                     PageRequests& requests,
                     SendFrame send,
                     SessionCookieJar& cookie_jar,
                     SessionHistory& history)
    : runner_(runner),
      requests_(requests),
      send_(std::move(send)),
      cookie_jar_(cookie_jar),
      history_(history) {}

PageFrame::~PageFrame() = default;

void PageFrame::ExpectOpeningNavigation() {
  opening_ = std::make_unique<DocumentLoader>(*runner_, *requests_, send_,
                                              *this, kNavigationRequest);
  requests_->SetNavigationClient(*opening_);
}

void PageFrame::BeginNavigation(const NavigationRequest& request) {
  const base::TimeTicks now = base::TimeTicks::Now();
  // RenderFrameImpl::BeginNavigationInternal(): a navigation the same as the
  // one that waits, which started less than 3 seconds before while no script
  // wrote a cookie, is dropped, and the one that waits goes on.
  if (provisional_ && request.url == provisional_request_.url &&
      request.reload == provisional_request_.reload &&
      request.user_activation == provisional_request_.user_activation &&
      now - provisional_start_ < kDuplicateWindow &&
      cookie_jar_->cookie_writes() == provisional_cookie_writes_) {
    return;
  }
  // Otherwise the newest wins, as the browser's FrameTreeNode takes the new
  // NavigationRequest, and the one that waited ends with ERR_ABORTED.
  if (provisional_) {
    requests_->Cancel(provisional_->number());
    provisional_.reset();
  }
  provisional_ =
      std::make_unique<DocumentLoader>(*runner_, *requests_, send_, *this);
  provisional_request_ = request;
  provisional_start_ = now;
  provisional_cookie_writes_ = cookie_jar_->cookie_writes();
  PageRequest page_request;
  page_request.kind = PageRequestKind::kNavigation;
  page_request.url = request.url.spec();
  page_request.user_activation = request.user_activation;
  page_request.reload = request.reload;
  page_request.referrer_policy = ToPageReferrerPolicy(request.referrer_policy);
  // A worker's thread may number a request in between, so the loader learns
  // its number once the request has it.
  provisional_->SetNumber(
      requests_->StartNavigation(std::move(page_request), *provisional_));
}

void PageFrame::DidCommitSameDocumentNavigation(const HistoryItem& item,
                                                FrameLoadType type) {
  history_->DidCommitSameDocumentNavigation(item, type);
  // A new entry discards the pending one, which a later go() no longer
  // counts from, as Chrome's controller discards it.
  if (type == FrameLoadType::kStandard) {
    pending_traversal_index_.reset();
  }
  NotifyNavigationApiOfDisposedEntries();
  const std::string& spec = item.url.spec();
  // A URL too long for a frame stays the Page's, which no Chrome would have.
  if (spec.size() <= kMaxPayloadSize - 8) {
    send_.Run(
        EncodeSameDocumentNavigation(Window::Current().DocumentId(), spec));
  }
}

void PageFrame::NotifyNavigationApiOfDisposedEntries() {
  std::vector<std::u16string> keys = history_->TakeRemovedKeys();
  if (keys.empty()) {
    return;
  }
  // From a later task, as Chrome's browser tells the renderer.
  runner_->PostTask(TaskType::kDOMManipulation,
                    base::BindOnce(
                        [](std::vector<std::u16string> keys) {
                          Window::Current()
                              .Navigation()
                              ->DisposeEntriesForSessionHistoryRemoval(keys);
                        },
                        std::move(keys)));
}

void PageFrame::DidUpdateCurrentHistoryItem(const HistoryItem& item) {
  history_->UpdateCurrentItem(item);
}

HistoryCounts PageFrame::GetHistoryCounts() const {
  return history_->Counts();
}

void PageFrame::NavigateBackForward(int offset) {
  // GoToOffsetFromRenderer(): from the entry that a waiting traversal goes
  // to, or the current one, and nowhere past the entries.
  const HistoryCounts counts = history_->Counts();
  const int index =
      (pending_traversal_index_ ? static_cast<int>(*pending_traversal_index_)
                                : counts.index) +
      offset;
  if (index < 0 || index >= counts.length) {
    return;
  }
  QueueTraversal(static_cast<size_t>(index), std::nullopt);
}

void PageFrame::NavigateToNavigationApiKey(const std::u16string& key) {
  const std::optional<size_t> index = history_->IndexOfKey(key);
  if (!index) {
    runner_->PostTask(TaskType::kDOMManipulation,
                      base::BindOnce(
                          [](std::u16string key) {
                            Window::Current().Navigation()->TraverseCancelled(
                                key,
                                Navigation::TraverseCancelledReason::kNotFound);
                          },
                          key));
    return;
  }
  QueueTraversal(*index, key);
}

void PageFrame::QueueTraversal(size_t index,
                               std::optional<std::u16string> key) {
  pending_traversal_index_ = index;
  ++waiting_traversals_;
  // Behind the tasks that the document has queued, hashchange among them, as
  // the browser's answer comes in Chrome.
  runner_->PostTask(
      TaskType::kDOMManipulation,
      base::BindOnce(&PageFrame::GoToItem, weak_factory_.GetWeakPtr(),
                     history_->RootAt(index).item().item_sequence_number,
                     std::move(key)));
}

void PageFrame::GoToItem(int64_t item_sequence_number,
                         std::optional<std::u16string> key) {
  if (--waiting_traversals_ == 0) {
    pending_traversal_index_.reset();
  }
  const std::optional<size_t> index =
      history_->IndexOfItem(item_sequence_number);
  if (!index) {
    if (key) {
      Window::Current().Navigation()->TraverseCancelled(
          *key, Navigation::TraverseCancelledReason::kAbortedBeforeCommit);
    }
    return;
  }
  if (!GoToIndex(*index) && key) {
    // A traversal that went nowhere ends the traverseTo() that asked for it.
    Window::Current().Navigation()->TraverseCancelled(
        *key, Navigation::TraverseCancelledReason::kAbortedBeforeCommit);
  }
}

bool PageFrame::GoToIndex(size_t index) {
  Window& window = Window::Current();
  const HistoryItem* const current = window.CurrentHistoryItem();
  if (!current || !window.HasContext()) {
    return false;
  }
  const HistoryItem target = history_->RootAt(index).item();
  if (target.document_sequence_number == current->document_sequence_number) {
    // The document commits it, and the history hears of it then.
    CommitSameDocumentHistoryNavigation(window, target);
    return true;
  }
  // An entry of another document: a Known Gap until traversals reach the
  // Session.
  return false;
}

bool PageFrame::HasProvisionalNavigation() const {
  return provisional_ != nullptr;
}

void PageFrame::StopAllLoaders() {
  if (current_) {
    current_->StopLoading();
  }
  if (provisional_) {
    requests_->Cancel(provisional_->number());
    provisional_.reset();
  }
}

Document& PageFrame::CommitNavigation(
    DocumentLoader& loader,
    const GURL& url,
    base::TimeTicks navigation_start,
    const PageConnection& connection,
    const std::optional<std::u16string>& xml_content_type) {
  std::unique_ptr<DocumentLoader> committing = Take(loader);
  Document& old_document = Window::Current().GetDocument();
  // How the navigation goes in the session history, the opening one in
  // place of the initial entry, and the entry it comes from.
  FrameLoadType type = FrameLoadType::kReplaceCurrentItem;
  if (committing->number() != kNavigationRequest) {
    type = provisional_request_.reload    ? FrameLoadType::kReload
           : provisional_request_.replace ? FrameLoadType::kReplaceCurrentItem
                                          : FrameLoadType::kStandard;
  }
  pending_traversal_index_.reset();
  const FetchReferrerPolicy old_referrer_policy =
      old_document.GetReferrerPolicy();
  const HistoryItem* const old_item = Window::Current().CurrentHistoryItem();
  const std::optional<HistoryItem> old_history_item =
      old_item ? std::optional<HistoryItem>(*old_item) : std::nullopt;
  if (old_document.IsInitialEmptyDocument()) {
    // FrameLoader::DetachDocument(), for a frame that has committed no
    // navigation yet: the unload events first, then the loading stops.
    old_document.DispatchUnloadEvents();
    if (current_) {
      current_->StopLoading();
    }
  } else {
    // LocalFrame::DetachImpl(), as RenderDocument swaps in a new frame for
    // each later document: the loading stops first, then the unload events.
    if (current_) {
      current_->StopLoading();
    }
    old_document.DispatchUnloadEvents();
  }
  current_ = std::move(committing);
  const GURL old_url(old_document.UrlForBinding());
  const base::TimeTicks unload_event_start = old_document.UnloadEventStart();
  const base::TimeTicks unload_event_end = old_document.UnloadEventEnd();
  Document& document = runner_->CommitDocument(
      current_->number(), url, navigation_start, connection, xml_content_type);
  // The old document's unload times go to a new one of its origin, as
  // Blink's UnloadEventTimingInfo carries them.
  DocumentLoadTiming& timing = runner_->document_load_timing();
  timing.unload_event_start = unload_event_start;
  timing.unload_event_end = unload_event_end;
  timing.has_same_origin_as_previous_document =
      url::IsSameOriginWith(old_url, url);
  timing.is_reload =
      current_->number() != kNavigationRequest && provisional_request_.reload;
  // The new document's entry, which the session history takes, before any of
  // its scripts runs. The isolate's current context is the old document's
  // again here.
  const HistoryItem item = HistoryItem::ForCommit(
      old_history_item ? &*old_history_item : nullptr, url, type,
      HistoryNavigationType::kDifferentDocument);
  // ProtectUrlInNavigationApi(): the old document's referrer policy keeps its
  // URLs from the documents after it.
  if (old_history_item &&
      (old_referrer_policy == FetchReferrerPolicy::kNoReferrer ||
       old_referrer_policy == FetchReferrerPolicy::kOrigin)) {
    history_->ProtectUrlsOfDocument(old_history_item->document_sequence_number);
  }
  Window& window = *document.DefaultView();
  Navigation::EntryArrays arrays =
      history_->DidCommitNavigation(item, type, window.DocumentId());
  // The new document's entries come from the history as it is, without the
  // entries that it dropped, which no other document shows.
  history_->TakeRemovedKeys();
  window.SetCurrentHistoryItem(item);
  window.Navigation()->InitializeForNewWindow(std::move(arrays));
  return document;
}

void PageFrame::NavigationEndedWithoutDocument(DocumentLoader& loader) {
  // The loader is on the stack below this, so it goes from a later task.
  base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE,
                                                             Take(loader));
}

std::unique_ptr<DocumentLoader> PageFrame::Take(DocumentLoader& loader) {
  if (opening_.get() == &loader) {
    return std::move(opening_);
  }
  CHECK_EQ(provisional_.get(), &loader);
  return std::move(provisional_);
}

}  // namespace mitto
