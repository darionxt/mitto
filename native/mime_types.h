// Which MIME types Chrome shows as a document, and which it downloads: what a
// navigation's response and a data: URL that a page navigates to are decided
// by, in the library and in the Page program both.
//
// Upstream: third_party/blink/common/mime_util/mime_util.cc
// Upstream: media/base/mime_util_internal.cc

#ifndef MITTO_MIME_TYPES_H_
#define MITTO_MIME_TYPES_H_

#include <string_view>

namespace mitto {

// blink::IsSupportedMimeType(): an image, a text, XML, JSON or script type,
// or audio or video that Chrome on Windows plays, compared without case. A
// navigation to a response of another type is a download, but for PDF, which
// Chrome's viewer shows.
bool IsSupportedMimeType(std::string_view mime_type);

// download_utils::MustDownload()'s rule for MHTML: multipart/related and
// message/rfc822, compared without case, are downloads of a response with
// headers, which Chrome renders only from file: and content: URLs.
bool MustDownloadMimeType(std::string_view mime_type);

// blink::IsSupportedImageMimeType(): a type that a decoder of Chrome's
// takes, JPEG XL among them, as kJXLImageFormat is on.
bool IsSupportedImageMimeType(std::string_view mime_type);

// blink::IsSupportedJavascriptMimeType(), compared without case.
bool IsSupportedJavaScriptMimeType(std::string_view mime_type);

// MIMETypeRegistry::IsSupportedFontMIMEType(): font/ and woff, woff2, otf,
// ttf or sfnt, in any case.
bool IsSupportedFontMimeType(std::string_view mime_type);

// MIMETypeRegistry::IsSupportedImagePrefixedMIMEType(), which a source
// element's type is held to: an image type, or SVG's.
bool IsSupportedImagePrefixedMimeType(std::string_view mime_type);

// The kind of document that Blink makes of a navigation's response, as
// DocumentInit::ComputeDocumentType() works it out from its MIME type, with
// JSON apart from other text, as DocumentInit::CreateDocument() makes a
// JSONDocument of it. A PDF, which Chrome's viewer shows, is HTML here, as a
// plugin that an external handler takes is.
enum class DocumentKind {
  kHtml,
  kXhtml,
  kSvg,
  kXml,
  kImage,
  kMedia,
  kText,
  kJson,
};
DocumentKind DocumentKindOf(std::string_view mime_type);

}  // namespace mitto

#endif  // MITTO_MIME_TYPES_H_
