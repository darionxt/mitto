// Chrome's detector of a document's encoding, for a document that names none:
// Compact Encoding Detection, which Chromium keeps in third_party/ced, run as
// Blink's TextEncodingDetector runs it.
//
// Upstream: third_party/blink/renderer/platform/text/text_encoding_detector.h
// Upstream: third_party/blink/renderer/platform/text/text_encoding_detector.cc

#ifndef MITTO_ENCODING_TEXT_ENCODING_DETECTOR_H_
#define MITTO_ENCODING_TEXT_ENCODING_DETECTOR_H_

#include <stdint.h>

#include <string>
#include <string_view>

#include "base/containers/span.h"

class GURL;

namespace mitto {

// Detects the encoding of `bytes`, with `hint_encoding_name`, the encoding of a
// parent frame or empty, and the top-level domain of `hint_url` as hints. The
// browser's language, `hint_user_language`, counts only for a local file, so
// that a page decodes alike everywhere. Sets `detected_encoding` to the
// canonical name of what it detected, or empty if it detected nothing the
// registry knows, and returns whether to use it: not if it detected UTF-8,
// unless the document is a local file, so that sites keep labelling UTF-8.
bool DetectTextEncoding(base::span<const uint8_t> bytes,
                        std::string_view hint_encoding_name,
                        const GURL& hint_url,
                        std::string_view hint_user_language,
                        std::string& detected_encoding);

}  // namespace mitto

#endif  // MITTO_ENCODING_TEXT_ENCODING_DETECTOR_H_
