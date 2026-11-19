// What Chrome's favicon driver fetches once the top-level document has
// loaded: the icon candidates that Blink's Document::IconURLs() gives for the
// favicon, of which the browser's FaviconHandler downloads the first, through
// the renderer's ImageDownloader, as a request of the kind favicon, and the
// next if no image comes of it. The favicon service keeps what it fetched, so
// the Page fetches an icon URL once.
//
// Upstream: third_party/blink/renderer/core/dom/document.cc
// Upstream: third_party/blink/renderer/core/dom/icon_url.cc
// Upstream: components/favicon/core/favicon_handler.cc
// Upstream: components/favicon/content/content_favicon_driver.cc

#ifndef MITTO_PAGE_FAVICON_LOADER_H_
#define MITTO_PAGE_FAVICON_LOADER_H_

#include <vector>

#include "url/gurl.h"

namespace mitto {

class Document;

// Document::IconURLs() for the favicon, best first: the last link with an
// icon of the favicon type among the head's children, with an href and media
// that matches, then the others from the last; or, for a document of HTTP or
// HTTPS with none, its origin's /favicon.ico.
std::vector<GURL> FaviconCandidates(const Document& document);

// ContentFaviconDriver::DocumentOnLoadCompletedInPrimaryMainFrame(): the
// first candidate of `document` that the Page has not fetched yet goes as a
// request of the kind favicon, and the next if no image comes of it. A data:
// icon fetches nothing.
void FetchFavicon(Document& document);

}  // namespace mitto

#endif  // MITTO_PAGE_FAVICON_LOADER_H_
