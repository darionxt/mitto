#include "mitto/page/page_frame.h"

#include <utility>

#include "base/check.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "mitto/dom/document.h"
#include "mitto/dom/window.h"
#include "mitto/page/document_loader.h"
#include "mitto/page/page_requests.h"
#include "mitto/page/script_runner.h"
#include "mitto/page/session_cookie_jar.h"

namespace mitto {
namespace {

// RenderFrameImpl's window for a navigation the same as the one that waits,
// which it drops: kNavigationDuplicateDetectionTimeWindow.
constexpr base::TimeDelta kDuplicateWindow = base::Milliseconds(3000);

}  // namespace

PageFrame::PageFrame(ScriptRunner& runner,
                     PageRequests& requests,
                     SendFrame send,
                     SessionCookieJar& cookie_jar)
    : runner_(runner),
      requests_(requests),
      send_(std::move(send)),
      cookie_jar_(cookie_jar) {}

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
  provisional_ = std::make_unique<DocumentLoader>(
      *runner_, *requests_, send_, *this, requests_->NextNumber());
  provisional_->set_referrer_source(
      GURL(Window::Current().GetDocument().UrlForBinding()));
  provisional_request_ = request;
  provisional_start_ = now;
  provisional_cookie_writes_ = cookie_jar_->cookie_writes();
  PageRequest page_request;
  page_request.kind = PageRequestKind::kNavigation;
  page_request.url = request.url.spec();
  page_request.user_activation = request.user_activation;
  page_request.reload = request.reload;
  const uint64_t number =
      requests_->StartNavigation(std::move(page_request), *provisional_);
  CHECK_EQ(number, provisional_->number());
}

void PageFrame::DidNavigateWithinDocument(const GURL& url) {
  const std::string& spec = url.spec();
  // A URL too long for a frame stays the Page's, which no Chrome would have.
  if (spec.size() <= kMaxPayloadSize - 8) {
    send_.Run(
        EncodeSameDocumentNavigation(Window::Current().DocumentId(), spec));
  }
}

Document& PageFrame::CommitNavigation(DocumentLoader& loader,
                                      const GURL& url,
                                      base::TimeTicks navigation_start,
                                      const PageConnection& connection) {
  std::unique_ptr<DocumentLoader> committing = Take(loader);
  Document& old_document = Window::Current().GetDocument();
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
  return runner_->CommitDocument(current_->number(), url, navigation_start,
                                 connection);
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
