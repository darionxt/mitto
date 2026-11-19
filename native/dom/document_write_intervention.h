// Chrome's intervention against a script of another site that blocks the
// parser and that document.write() inserts, which can delay a page on a slow
// connection: Chrome warns in the console of every such script, and blocks
// its fetch where its settings say so.
//
// Chrome's settings block it only on a 2G cellular connection, which a
// desktop never reports, so Chrome on Windows fetches it as any other, even
// on a connection that is effectively 2G, and only warns. The
// effectively-2G setting and the one that blocks on any connection are off,
// unless a field trial turns one on.
//
// Blink's parser checks each such script twice in a write that leaves the
// parser waiting: once as it fetches it, and once as the preload scanner of
// what was written finds it (html_preload_scanner.h), so the console says the
// warning twice.
//
// Upstream: third_party/blink/renderer/core/script/document_write_intervention.cc
// Upstream: third_party/blink/renderer/core/frame/settings.json5

#ifndef MITTO_DOM_DOCUMENT_WRITE_INTERVENTION_H_
#define MITTO_DOM_DOCUMENT_WRITE_INTERVENTION_H_

class GURL;

namespace mitto {

class Document;

// MaybeDisallowFetchForDocWrittenScript(): warns of `url`, a script that
// `document` fetches while it is written, unless it is `deferred` (async,
// deferred or not inserted by the parser), is not HTTP or HTTPS, or is of the
// document's host or registrable domain. A Page's document of a window is its
// main frame's.
void MaybeDisallowFetchForDocWrittenScript(const GURL& url,
                                           bool deferred,
                                           Document& document);

}  // namespace mitto

#endif  // MITTO_DOM_DOCUMENT_WRITE_INTERVENTION_H_
