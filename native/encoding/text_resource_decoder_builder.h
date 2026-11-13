// Makes the decoder of a document's body as Blink's BuildTextResourceDecoder()
// does for a frame: the encoding that the response's Content-Type names, or a
// same-origin parent frame's, or else what the document declares or Chrome's
// detector finds, with Chrome's default for the top-level domain of the URL or
// for the browser's locale when nothing does.
//
// Upstream: third_party/blink/renderer/core/html/parser/text_resource_decoder_builder.h
// Upstream: third_party/blink/renderer/core/html/parser/text_resource_decoder_builder.cc
// Upstream: third_party/blink/renderer/platform/network/mime/mime_type_registry.cc
// Upstream: chrome/browser/ui/prefs/prefs_tab_helper.cc
// Upstream: components/components_locale_settings.grd

#ifndef MITTO_ENCODING_TEXT_RESOURCE_DECODER_BUILDER_H_
#define MITTO_ENCODING_TEXT_RESOURCE_DECODER_BUILDER_H_

#include <memory>
#include <string>
#include <string_view>

#include "mitto/encoding/text_resource_decoder.h"

class GURL;

namespace mitto {

// The encoding of a parent frame's document, for a frame that can access it.
struct ParentFrameEncoding {
  std::string encoding;
  // Whether the parent's encoding came from its byte order mark or from the
  // detector, so that the frame's detector takes it as a hint.
  bool detected_heuristically = false;
};

// The canonical name of the encoding that Chrome decodes a document with when
// nothing else says, for its user interface in `locale`, a BCP 47 tag such as
// "en-US": the default of Chrome's setting "intl.charset_default", which each
// locale's strings give, windows-1252 for most.
std::string DefaultTextEncodingForLocale(std::string_view locale);

// Blink's MIMETypeRegistry::IsXMLMIMEType() and IsJSONMimeType(), for a MIME
// type without parameters.
bool IsXmlMimeType(std::string_view mime_type);
bool IsJsonMimeType(std::string_view mime_type);

// The decoder of the body of a document at `url` whose response has
// `mime_type` and `charset`, both as //net parses Content-Type
// (content_type.h), in a browser whose user interface is in `locale`.
// `parent` is the parent frame's encoding, for a frame that can access its
// parent, and null otherwise.
std::unique_ptr<TextResourceDecoder> BuildTextResourceDecoder(
    const GURL& url,
    std::string_view mime_type,
    std::string_view charset,
    std::string_view locale,
    const ParentFrameEncoding* parent);

// The decoder of a classic script's body, as Blink's ScriptResource decodes
// it: as `encoding`, which the element's charset attribute names or else its
// document's encoding, unless the response's `charset` or a byte order mark
// names another.
std::unique_ptr<TextResourceDecoder> BuildClassicScriptDecoder(
    std::string_view encoding,
    std::string_view charset);

}  // namespace mitto

#endif  // MITTO_ENCODING_TEXT_RESOURCE_DECODER_BUILDER_H_
