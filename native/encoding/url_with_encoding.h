// Resolves a URL that a document gives, such as a script's src, with its query
// in the document's encoding, as the URL Standard has it and Blink's
// KURL(base, relative, encoding) does for Document::CompleteURL(): each
// character of the query that the encoding has becomes its bytes, and one it
// lacks becomes "&#" its code point ";", percent-encoded as "%26%23...%3B".
// The path and the fragment stay UTF-8.
//
// Upstream: third_party/blink/renderer/platform/weborigin/kurl.cc

#ifndef MITTO_ENCODING_URL_WITH_ENCODING_H_
#define MITTO_ENCODING_URL_WITH_ENCODING_H_

#include <string>
#include <string_view>

#include "url/gurl.h"

namespace mitto {

// `relative` resolved against `base`, with the query in `encoding`, a
// canonical name, or in UTF-8 if it is empty, UTF-8 or UTF-16, which the URL
// Standard encodes as UTF-8. An invalid GURL if it does not resolve.
GURL ResolveUrlWithEncoding(const GURL& base,
                            std::u16string_view relative,
                            std::string_view encoding);

}  // namespace mitto

#endif  // MITTO_ENCODING_URL_WITH_ENCODING_H_
