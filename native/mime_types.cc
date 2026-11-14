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

}  // namespace

bool IsSupportedMimeType(std::string_view mime_type) {
  const std::string lower = base::ToLowerASCII(mime_type);
  return kSupportedImageTypes.contains(lower) ||
         kSupportedNonImageTypes.contains(lower) ||
         kSupportedJavascriptTypes.contains(lower) ||
         kSupportedMediaTypes.contains(lower) || kPdfTypes.contains(lower) ||
         (lower.starts_with("text/") &&
          !kUnsupportedTextTypes.contains(lower)) ||
         (lower.starts_with("application/") && lower.ends_with("+json"));
}

}  // namespace mitto
