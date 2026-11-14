// What a window's navigations ask of the Page program: a navigation of the
// window's frame to another document, which the Session makes, and word of a
// navigation within the document. The Page program gives the window one, and
// the DOM knows nothing of how it reaches the Session, as with the Fetcher.
//
// Upstream: third_party/blink/renderer/core/frame/local_frame_client.h

#ifndef MITTO_DOM_NAVIGATION_CLIENT_H_
#define MITTO_DOM_NAVIGATION_CLIENT_H_

#include "url/gurl.h"

namespace mitto {

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
};

class NavigationClient {
 public:
  // LocalFrameClient::BeginNavigation(), once the window's document has let
  // the navigation go on: the Session starts it, and its response, unless it
  // has no document, takes the place of the window's document.
  virtual void BeginNavigation(const NavigationRequest& request) = 0;

  // The Session hears that a navigation within the window's document gave it
  // `url`, as Blink tells its browser with DidCommitSameDocumentNavigation().
  virtual void DidNavigateWithinDocument(const GURL& url) = 0;

 protected:
  virtual ~NavigationClient() = default;
};

}  // namespace mitto

#endif  // MITTO_DOM_NAVIGATION_CLIENT_H_
