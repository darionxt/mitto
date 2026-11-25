#include "mitto/mime_types.h"

#include <string>
#include <string_view>

#include "base/containers/fixed_flat_set.h"
#include "base/strings/string_util.h"

namespace mitto {
namespace {

// kSupportedImageTypes, with AVIF, which dav1d decodes.
constexpr auto kSupportedImageTypes = base::MakeFixedFlatSet<std::string_view>({
    "image/jpeg",
    "image/pjpeg",
    "image/jpg",
    "image/webp",
    "image/png",
    "image/apng",
    "image/gif",
    "image/bmp",
    "image/vnd.microsoft.icon",
    "image/x-icon",
    "image/x-xbitmap",
    "image/x-png",
    "image/avif",
});

constexpr auto kSupportedJavascriptTypes =
    base::MakeFixedFlatSet<std::string_view>({
        "application/ecmascript",
        "application/javascript",
        "application/x-ecmascript",
        "application/x-javascript",
        "text/ecmascript",
        "text/javascript",
        "text/javascript1.0",
        "text/javascript1.1",
        "text/javascript1.2",
        "text/javascript1.3",
        "text/javascript1.4",
        "text/javascript1.5",
        "text/jscript",
        "text/livescript",
        "text/x-ecmascript",
        "text/x-javascript",
    });

constexpr auto kUnsupportedTextTypes =
    base::MakeFixedFlatSet<std::string_view>({
        "text/calendar",
        "text/x-calendar",
        "text/x-vcalendar",
        "text/vcalendar",
        "text/vcard",
        "text/x-vcard",
        "text/directory",
        "text/ldif",
        "text/qif",
        "text/x-qif",
        "text/x-csv",
        "text/x-vcf",
        "text/rtf",
        "text/comma-separated-values",
        "text/csv",
        "text/tab-separated-values",
        "text/tsv",
        "text/ofx",
        "text/vnd.sun.j2me.app-descriptor",
        "text/x-ms-iqy",
        "text/x-ms-odc",
        "text/x-ms-rqy",
        "text/x-ms-contact",
    });

constexpr auto kSupportedNonImageTypes =
    base::MakeFixedFlatSet<std::string_view>({
        "image/svg+xml",
        "application/xml",
        "application/atom+xml",
        "application/rss+xml",
        "application/xhtml+xml",
        "application/json",
        "message/rfc822",
        "multipart/related",
        "multipart/x-mixed-replace",
    });

// What MimeUtil::AddSupportedMediaFormats() adds on Windows, where Chrome has
// the proprietary codecs and the HLS demuxer.
constexpr auto kSupportedMediaTypes = base::MakeFixedFlatSet<std::string_view>({
    "audio/wav",
    "audio/x-wav",
    "audio/webm",
    "video/webm",
    "audio/ogg",
    "video/ogg",
    "application/ogg",
    "audio/flac",
    "audio/mpeg",
    "audio/mp3",
    "audio/x-mp3",
    "audio/mp4",
    "video/mp4",
    "audio/matroska",
    "video/matroska",
    "audio/x-matroska",
    "video/x-matroska",
    "audio/aac",
    "audio/x-m4a",
    "video/x-m4v",
    "video/3gpp",
    "application/x-mpegurl",
    "application/vnd.apple.mpegurl",
    "audio/mpegurl",
    "audio/x-mpegurl",
});

// The types that Chrome's PDF viewer takes, as a plugin would.
constexpr auto kPdfTypes = base::MakeFixedFlatSet<std::string_view>({
    "application/pdf",
    "text/pdf",
});

// MIMETypeRegistry::IsXMLMIMEType(): text/xml, application/xml, or a type of
// RFC 3023's form that ends in +xml.
bool IsXmlMimeType(std::string_view mime_type) {
  if (mime_type == "text/xml" || mime_type == "application/xml") {
    return true;
  }
  const size_t length = mime_type.size();
  if (length < 7 || mime_type[0] == '/' || mime_type[length - 5] == '/' ||
      !mime_type.ends_with("+xml")) {
    return false;
  }
  bool has_slash = false;
  for (size_t i = 0; i < length - 4; ++i) {
    const char c = mime_type[i];
    if (base::IsAsciiAlphaNumeric(c) ||
        std::string_view("_-+~!$^{}|.%'`#&*").find(c) !=
            std::string_view::npos) {
      continue;
    }
    if (c != '/' || has_slash) {
      return false;
    }
    has_slash = true;
  }
  return true;
}

// blink::IsJSONMimeType(): application/json, text/json, or a type whose
// subtype ends in +json.
bool IsJsonMimeType(std::string_view mime_type) {
  if (mime_type == "application/json" || mime_type == "text/json") {
    return true;
  }
  const size_t slash = mime_type.find('/');
  return slash != std::string_view::npos && slash > 0 &&
         mime_type.find('/', slash + 1) == std::string_view::npos &&
         mime_type.substr(slash + 1).ends_with("+json") &&
         mime_type.size() > slash + 1 + 5;
}

}  // namespace

bool IsSupportedImageMimeType(std::string_view mime_type) {
  const std::string lower = base::ToLowerASCII(mime_type);
  return lower == "image/jxl" || kSupportedImageTypes.contains(lower);
}

bool IsSupportedJavaScriptMimeType(std::string_view mime_type) {
  return kSupportedJavascriptTypes.contains(base::ToLowerASCII(mime_type));
}

bool IsSupportedFontMimeType(std::string_view mime_type) {
  const std::string lower = base::ToLowerASCII(mime_type);
  if (!lower.starts_with("font/")) {
    return false;
  }
  const std::string_view sub_type = std::string_view(lower).substr(5);
  return sub_type == "woff" || sub_type == "woff2" || sub_type == "otf" ||
         sub_type == "ttf" || sub_type == "sfnt";
}

bool IsSupportedImagePrefixedMimeType(std::string_view mime_type) {
  const std::string lower = base::ToLowerASCII(mime_type);
  return IsSupportedImageMimeType(lower) ||
         (lower.starts_with("image/") &&
          kSupportedNonImageTypes.contains(lower));
}

DocumentKind DocumentKindOf(std::string_view mime_type) {
  const std::string lower = base::ToLowerASCII(mime_type);
  if (lower == "text/html") {
    return DocumentKind::kHtml;
  }
  if (lower == "application/xhtml+xml") {
    return DocumentKind::kXhtml;
  }
  if (lower == "image/svg+xml") {
    return DocumentKind::kSvg;
  }
  if (IsSupportedImageMimeType(lower) || lower == "multipart/x-mixed-replace") {
    return DocumentKind::kImage;
  }
  if (kSupportedMediaTypes.contains(lower)) {
    return DocumentKind::kMedia;
  }
  if (IsJsonMimeType(lower)) {
    return DocumentKind::kJson;
  }
  // MIMETypeRegistry::IsPlainTextMIMEType() and JavaScript's types.
  if (kSupportedJavascriptTypes.contains(lower) ||
      (lower.starts_with("text/") && lower != "text/html" &&
       lower != "text/xml" && lower != "text/xsl")) {
    return DocumentKind::kText;
  }
  if (IsXmlMimeType(lower)) {
    return DocumentKind::kXml;
  }
  return DocumentKind::kHtml;
}

bool MustDownloadMimeType(std::string_view mime_type) {
  return base::EqualsCaseInsensitiveASCII(mime_type, "multipart/related") ||
         base::EqualsCaseInsensitiveASCII(mime_type, "message/rfc822");
}

bool IsSupportedMimeType(std::string_view mime_type) {
  const std::string lower = base::ToLowerASCII(mime_type);
  return IsSupportedImageMimeType(lower) ||
         kSupportedNonImageTypes.contains(lower) ||
         kSupportedJavascriptTypes.contains(lower) ||
         kSupportedMediaTypes.contains(lower) || kPdfTypes.contains(lower) ||
         (lower.starts_with("text/") &&
          !kUnsupportedTextTypes.contains(lower)) ||
         (lower.starts_with("application/") && lower.ends_with("+json"));
}

}  // namespace mitto
