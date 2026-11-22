// The Page's one frame, its top-level one, on the Page program's main thread:
// the loader of its document and that of the navigation that waits for its
// response, as Blink's FrameLoader and content's RenderFrameImpl keep them.
// It is each window's NavigationClient: a navigation that a document starts
// goes to the Session as a request of Kind navigation, and its response,
// unless it has none, takes the place of the document once the old one has
// unloaded, as Blink's FrameLoader::CommitNavigation() has it.
//
// Upstream: third_party/blink/renderer/core/loader/frame_loader.cc
// Upstream: third_party/blink/renderer/core/frame/local_frame.cc
// Upstream: content/renderer/render_frame_impl.cc

#ifndef MITTO_PAGE_PAGE_FRAME_H_
#define MITTO_PAGE_PAGE_FRAME_H_

#include <stdint.h>

#include <memory>
#include <optional>
#include <string>

#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "mitto/dom/navigation_client.h"
#include "mitto/page/page_reporter.h"
#include "mitto/page_messages.h"
#include "url/gurl.h"

namespace mitto {

class Document;
class DocumentLoader;
class PageRequests;
class ScriptRunner;
class SessionCookieJar;
class SessionHistory;

class PageFrame final : public NavigationClient {
 public:
  PageFrame(ScriptRunner& runner,
            PageRequests& requests,
            SendFrame send,
            SessionCookieJar& cookie_jar,
            SessionHistory& history);

  PageFrame(const PageFrame&) = delete;
  PageFrame& operator=(const PageFrame&) = delete;

  ~PageFrame() override;

  // Has the navigation that the Session makes when the Page opens load the
  // frame's document, if it makes one.
  void ExpectOpeningNavigation();

  // NavigationClient:
  void BeginNavigation(const NavigationRequest& request) override;
  void DidCommitSameDocumentNavigation(const HistoryItem& item,
                                       FrameLoadType type) override;
  void DidUpdateCurrentHistoryItem(const HistoryItem& item) override;
  HistoryCounts GetHistoryCounts() const override;
  void NavigateBackForward(int offset) override;
  void NavigateToNavigationApiKey(const std::u16string& key) override;
  bool HasProvisionalNavigation() const override;
  void StopAllLoaders() override;

  // What a loader of the frame calls.
  //
  // The response of `loader`'s navigation has come: the frame's document
  // stops loading and unloads, and a new context's document at `url` takes
  // its place, which this returns: an XMLDocument of `xml_content_type`
  // where one is given. `loader` becomes the document's.
  Document& CommitNavigation(
      DocumentLoader& loader,
      const GURL& url,
      base::TimeTicks navigation_start,
      const PageConnection& connection,
      const std::optional<std::u16string>& xml_content_type = std::nullopt);
  // `loader`'s navigation ended with no response, and the frame keeps its
  // document. `loader` goes.
  void NavigationEndedWithoutDocument(DocumentLoader& loader);

 private:
  // Takes `loader` from where the frame holds it, as the opening navigation's
  // or the one that waits.
  std::unique_ptr<DocumentLoader> Take(DocumentLoader& loader);
  // NavigationControllerImpl::GoToOffsetFromRenderer() and
  // NavigateToNavigationApiKey(), from the task that the browser's would run
  // in: the target is worked out from where the history is then.
  void GoToOffset(int offset);
  void GoToKey(const std::u16string& key);
  // NotifyNavigationApiOfDisposedEntries(): the document hears of the
  // entries that the session history dropped.
  void NotifyNavigationApiOfDisposedEntries();
  // NavigateToExistingPendingEntry() for the top-level frame: a traversal to
  // entry `index`, within the document if it is of the document's.
  void GoToIndex(size_t index);

  const raw_ref<ScriptRunner> runner_;
  const raw_ref<PageRequests> requests_;
  const SendFrame send_;
  const raw_ref<SessionCookieJar> cookie_jar_;
  const raw_ref<SessionHistory> history_;
  // The loader of the frame's document, none for about:blank.
  std::unique_ptr<DocumentLoader> current_;
  // The navigation that the Session makes when the Page opens, until it ends.
  std::unique_ptr<DocumentLoader> opening_;
  // The navigation that a document started, while it waits for its response,
  // and what RenderFrameImpl compares a new one with to drop it as the same.
  std::unique_ptr<DocumentLoader> provisional_;
  NavigationRequest provisional_request_;
  base::TimeTicks provisional_start_;
  uint64_t provisional_cookie_writes_ = 0;
  base::WeakPtrFactory<PageFrame> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_PAGE_PAGE_FRAME_H_
