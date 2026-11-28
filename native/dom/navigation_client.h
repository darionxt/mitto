// What a window's navigations ask of the Page program: a navigation of the
// window's frame to another document, which the Session makes, word of a
// navigation within the document, and the session history that history and
// the Navigation API read and traverse. The Page program gives the window
// one, and the DOM knows nothing of how it reaches the Session, as with the
// Fetcher.
//
// Upstream: third_party/blink/renderer/core/frame/local_frame_client.h

#ifndef MITTO_DOM_NAVIGATION_CLIENT_H_
#define MITTO_DOM_NAVIGATION_CLIENT_H_

#include <string>

#include "mitto/dom/fetcher.h"
#include "mitto/dom/frame_loader.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace mitto {

class Window;

// What a navigation to another document that a document starts says of
// itself, as Blink's WebNavigationInfo does. Its Initiator is the document,
// which the Session works out.
struct NavigationRequest {
  GURL url;
  // The frame had transient user activation, so Sec-Fetch-User goes with it.
  bool user_activation = false;
  // location.reload() and a refresh to the document's URL, which validate
  // what the cache holds.
  bool reload = false;
  // One that replaces the current entry of the session history.
  bool replace = false;
  // The referrer policy of the document that starts it.
  FetchReferrerPolicy referrer_policy = FetchReferrerPolicy::kDefault;
  // The document that starts it, its initiator, by its ID in the Page, and
  // its origin and URL, which an about:blank that it commits takes, as its
  // origin and its referrer.
  uint64_t initiator = 0;
  url::Origin initiator_origin;
  GURL initiator_url;
  // The initiator's base URL and cookie URL, which an about:blank that it
  // commits takes as its about base URL and its cookie URL.
  GURL initiator_base_url;
  GURL initiator_cookie_url;
  // The initiator's own referrer policy, which an about:blank that it commits
  // takes with its policy container, beside the navigation's above.
  FetchReferrerPolicy initiator_referrer_policy = FetchReferrerPolicy::kDefault;
  // The initiator's outgoing referrer, as Blink's Document::OutgoingReferrer()
  // gives it: its document's URL, or for a srcdoc document its parent's,
  // stripped for use as a referrer, and empty for an opaque origin.
  GURL initiator_referrer;
  // Whether that, which is taken as the navigation starts, is another than the
  // initiator has as it leaves, so its request names it, or names none.
  bool names_referrer = false;
  // Whether the initiator is in a process for sandboxed documents, which an
  // about:blank that it commits is in too.
  bool initiator_in_sandboxed_process = false;
  // Whether a header gave the initiator a Content-Security-Policy, which a
  // local URL's document that it commits takes with its policy container.
  bool initiator_has_header_csp = false;
  // Whether the initiator's embedder policy, which such a document takes too,
  // is compatible with cross-origin isolation, and whether its document
  // isolation policy, which it takes too, isolates it.
  bool initiator_embedder_policy_isolates = false;
  bool initiator_isolated_by_document_policy = false;
};

struct HistoryItem;

// Where the window's frame is in the session history, as Blink's WebViewImpl
// counts it for history.length and history.go()'s checks: the current entry's
// index and how many entries there are.
struct HistoryCounts {
  int index = 0;
  int length = 1;
};

class NavigationClient {
 public:
  // LocalFrameClient::BeginNavigation(), once the window's document has let
  // the navigation go on: the Session starts it, and its response, unless it
  // has no document, takes the place of the window's document.
  virtual void BeginNavigation(const NavigationRequest& request) = 0;

  // A navigation within the window's document gave it the entry `item`: a
  // new one for kStandard, one in place of the current one for
  // kReplaceCurrentItem, or for kBackForward the one that the session history
  // went to. The session history takes it, and the Session hears of its URL,
  // as Blink tells its browser with DidCommitSameDocumentNavigation().
  virtual void DidCommitSameDocumentNavigation(const HistoryItem& item,
                                               FrameLoadType type) = 0;
  // The current entry changed in place, its scrollRestoration or its
  // Navigation API state, as Blink's DidUpdateCurrentHistoryItem() and
  // NotifyCurrentHistoryItemChanged() say.
  virtual void DidUpdateCurrentHistoryItem(const HistoryItem& item) = 0;

  // LocalFrameClient::BackForwardLength() and the counts behind it.
  virtual HistoryCounts GetHistoryCounts() const = 0;
  // GoToEntryAtOffset(): the session history goes `offset` entries back or
  // forward from where it is when a later task runs, as Chrome's browser
  // takes the renderer's message.
  virtual void NavigateBackForward(int offset) = 0;
  // NavigateToNavigationApiKey(): the same, to the frame's entry of `key`.
  virtual void NavigateToNavigationApiKey(const std::u16string& key) = 0;
  // The Session hears that the document of `opener`, another window of the
  // window's document's origin, opened that document with document.open(),
  // as Blink tells its browser with DidOpenDocumentInputStream().
  virtual void DidOpenDocument(const Window& opener) = 0;

  // FrameLoader::HasProvisionalNavigation(): a navigation that a document
  // started waits for its response.
  virtual bool HasProvisionalNavigation() const = 0;
  // The rest of FrameLoader::StopAllLoaders(), as document.open() runs it
  // once the document has cancelled its parsing: the document's loading
  // stops, and so does the navigation that waits, which ends with
  // ERR_ABORTED.
  virtual void StopAllLoaders() = 0;

 protected:
  virtual ~NavigationClient() = default;
};

}  // namespace mitto

#endif  // MITTO_DOM_NAVIGATION_CLIENT_H_
