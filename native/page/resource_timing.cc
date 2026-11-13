#include "mitto/page/resource_timing.h"

#include <stddef.h>

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/dom/performance_server_timing.h"
#include "url/gurl.h"

namespace mitto {
namespace {

// Blink's MIME lists.
constexpr std::array<std::string_view, 13> kSupportedImageTypes = {
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
    // ENABLE_DAV1D_DECODER, as in every desktop Chrome.
    "image/avif",
};

constexpr std::array<std::string_view, 16> kSupportedJavascriptTypes = {
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
};

constexpr std::array<std::string_view, 23> kUnsupportedTextTypes = {
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
};

constexpr std::array<std::string_view, 9> kSupportedNonImageTypes = {
    "image/svg+xml",       "application/xml",       "application/atom+xml",
    "application/rss+xml", "application/xhtml+xml", "application/json",
    "message/rfc822",      "multipart/related",     "multipart/x-mixed-replace",
};

// media::MimeUtil::AddSupportedMediaFormats() for Chrome on desktop, which has
// the proprietary codecs and the HLS demuxer.
constexpr std::array<std::string_view, 25> kSupportedMediaTypes = {
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
};

// The content codings Chrome decodes.
constexpr std::array<std::string_view, 6> kSupportedContentEncodings = {
    "br", "dcb", "dcz", "deflate", "gzip", "zstd",
};

template <size_t N>
bool Contains(const std::array<std::string_view, N>& list,
              std::string_view value) {
  return !value.empty() && std::ranges::find(list, value) != list.end();
}

// HttpUtil::IsToken().
// Upstream: net/http/http_util.cc
bool IsToken(std::string_view text) {
  if (text.empty()) {
    return false;
  }
  for (const char c : text) {
    const unsigned char byte = static_cast<unsigned char>(c);
    if (byte >= 0x7F || byte <= 0x20 ||
        std::string_view("()<>@,;:\\\"/[]?={}").find(c) !=
            std::string_view::npos) {
      return false;
    }
  }
  return true;
}

// net::MatchesMimeType() for a pattern of one wildcard and no parameters, of a
// type without parameters: `left`, anything, then `right`.
bool MatchesWildcard(std::string_view left,
                     std::string_view right,
                     std::string_view mime_type,
                     bool slash_and_tokens) {
  if (slash_and_tokens) {
    const size_t slash = mime_type.find('/');
    if (slash == std::string_view::npos ||
        !IsToken(mime_type.substr(0, slash)) ||
        !IsToken(mime_type.substr(slash + 1))) {
      return false;
    }
  }
  return mime_type.size() >= left.size() + right.size() &&
         base::StartsWith(mime_type, left,
                          base::CompareCase::INSENSITIVE_ASCII) &&
         base::EndsWith(mime_type, right, base::CompareCase::INSENSITIVE_ASCII);
}

// IsJSONMimeType(): https://mimesniff.spec.whatwg.org/#json-mime-type
bool IsJsonMimeType(std::string_view mime_type) {
  return base::EqualsCaseInsensitiveASCII(mime_type, "application/json") ||
         base::EqualsCaseInsensitiveASCII(mime_type, "text/json") ||
         MatchesWildcard("", "+json", mime_type, /*slash_and_tokens=*/true);
}

// IsXMLMimeType() without SpecCompliantXmlMimeTypes, which is experimental.
bool IsXmlMimeType(std::string_view mime_type) {
  return base::EqualsCaseInsensitiveASCII(mime_type, "text/xml") ||
         base::EqualsCaseInsensitiveASCII(mime_type, "application/xml") ||
         MatchesWildcard("application/", "+xml", mime_type,
                         /*slash_and_tokens=*/false);
}

// IsSupportedMimeType(): an image type that Chrome decodes, or a type that it
// shows as something other than a download.
bool IsSupportedMimeType(std::string_view mime_type) {
  const std::string lower = base::ToLowerASCII(mime_type);
  if (lower.starts_with("image/") && Contains(kSupportedImageTypes, lower)) {
    return true;
  }
  return Contains(kSupportedNonImageTypes, lower) ||
         Contains(kSupportedJavascriptTypes, lower) ||
         Contains(kSupportedMediaTypes, lower) ||
         (lower.starts_with("text/") &&
          !Contains(kUnsupportedTextTypes, lower)) ||
         (lower.starts_with("application/") &&
          MatchesWildcard("application/", "+json", lower,
                          /*slash_and_tokens=*/false));
}

}  // namespace

std::string HeaderField(std::string_view headers, std::string_view name) {
  std::vector<std::string_view> values;
  for (const std::string_view line : base::SplitStringPiece(
           headers, "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    const size_t colon = line.find(':');
    if (colon != std::string_view::npos &&
        base::EqualsCaseInsensitiveASCII(line.substr(0, colon), name)) {
      values.push_back(
          base::TrimString(line.substr(colon + 1), " \t", base::TRIM_ALL));
    }
  }
  return base::JoinString(values, ", ");
}

std::string HttpContentType(std::string_view headers) {
  std::string mime_type;
  const std::string field =
      base::ToLowerASCII(HeaderField(headers, "content-type"));
  for (const std::string_view value : base::SplitStringPiece(
           field, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    const std::string_view type = base::TrimString(
        value.substr(0, value.find(';')), " \t", base::TRIM_ALL);
    if (type.find('/') != std::string_view::npos && type != "*/*") {
      mime_type = std::string(type);
    }
  }
  return mime_type;
}

std::string MinimizedMimeType(std::string_view mime_type) {
  if (Contains(kSupportedJavascriptTypes, mime_type)) {
    return "text/javascript";
  }
  if (IsJsonMimeType(mime_type)) {
    return "application/json";
  }
  if (base::EqualsCaseInsensitiveASCII(mime_type, "image/svg+xml")) {
    return "image/svg+xml";
  }
  if (IsXmlMimeType(mime_type)) {
    return "application/xml";
  }
  if (IsSupportedMimeType(mime_type)) {
    return std::string(mime_type);
  }
  return std::string();
}

std::string FilteredContentEncoding(std::string_view headers) {
  const std::string encoding =
      base::ToLowerASCII(HeaderField(headers, "content-encoding"));
  if (encoding.empty()) {
    return std::string();
  }
  if (Contains(kSupportedContentEncodings, encoding)) {
    return encoding;
  }
  if (encoding.find(',') != std::string::npos) {
    return "multiple";
  }
  return "@unknown";
}

base::TimeTicks TicksOf(int64_t microseconds) {
  return microseconds == 0
             ? base::TimeTicks()
             : base::TimeTicks() + base::Microseconds(microseconds);
}

ResourceTimingInfo CreateResourceTimingInfo(base::TimeTicks start_time,
                                            std::u16string initial_url,
                                            const PageResponseStarted* response,
                                            base::TimeTicks response_end) {
  ResourceTimingInfo info;
  info.start_time = start_time;
  info.name = std::move(initial_url);
  info.response_end = response_end;
  if (!response) {
    return info;
  }
  if (response->timing_allow_passed) {
    info.allow_timing_details = true;
    info.server_timing = ParseServerTimingHeader(
        HeaderField(response->headers, "server-timing"));
    info.is_secure_transport = GURL(response->url).SchemeIsCryptographic();
    info.has_timing = true;
    const PageLoadTiming& timing = response->timing;
    info.timing.request_start = TicksOf(timing.request_start);
    info.timing.domain_lookup_start = TicksOf(timing.domain_lookup_start);
    info.timing.domain_lookup_end = TicksOf(timing.domain_lookup_end);
    info.timing.connect_start = TicksOf(timing.connect_start);
    info.timing.connect_end = TicksOf(timing.connect_end);
    info.timing.ssl_start = TicksOf(timing.ssl_start);
    info.timing.send_start = TicksOf(timing.send_start);
    info.timing.receive_headers_start = TicksOf(timing.receive_headers_start);
    info.timing.receive_headers_end = TicksOf(timing.receive_headers_end);
    info.timing.receive_non_informational_headers_start =
        TicksOf(timing.receive_non_informational_headers_start);
    info.alpn_negotiated_protocol =
        base::ASCIIToUTF16(response->alpn_negotiated_protocol);
    info.connection_info = base::ASCIIToUTF16(response->connection_info);
    info.did_reuse_connection = timing.socket_reused;
  }
  // Status, type and coding are for a response of basic or cors tainting.
  if (!response->opaque) {
    info.response_status = static_cast<uint16_t>(response->status);
    info.content_type = base::ASCIIToUTF16(
        MinimizedMimeType(HttpContentType(response->headers)));
    info.content_encoding =
        base::ASCIIToUTF16(FilteredContentEncoding(response->headers));
  }
  return info;
}

void SetResourceTimingSizes(ResourceTimingInfo& info,
                            const PageResponseStarted& response,
                            int64_t encoded_body_size,
                            int64_t received_bytes,
                            uint64_t decoded_body_size) {
  if (!info.allow_timing_details) {
    return;
  }
  // ResourceResponse::CacheState().
  info.cache_state = response.validated    ? CacheState::kValidated
                     : received_bytes == 0 ? CacheState::kLocal
                                           : CacheState::kNone;
  // Without ResourceTimingUseCORSForBodySizes, which is a test feature, the
  // sizes go with the timing.
  info.encoded_body_size = static_cast<uint64_t>(encoded_body_size);
  info.decoded_body_size = decoded_body_size;
}

}  // namespace mitto
