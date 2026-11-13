#include "mitto/encoding/text_resource_decoder_builder.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "mitto/encoding/text_encoding_registry.h"
#include "url/gurl.h"

namespace mitto {

namespace {

using ContentType = TextResourceDecoderOptions::ContentType;

struct LegacyEncoding {
  std::string_view domain;
  std::string_view encoding;
};

// The encoding of pages of a top-level domain that Blink takes as their
// default before the browser's.
constexpr auto kEncodings = std::to_array<LegacyEncoding>(
    {{"au", "windows-1252"}, {"az", "ISO-8859-9"},   {"bd", "windows-1252"},
     {"bg", "windows-1251"}, {"br", "windows-1252"}, {"ca", "windows-1252"},
     {"ch", "windows-1252"}, {"cn", "GBK"},          {"cz", "windows-1250"},
     {"de", "windows-1252"}, {"dk", "windows-1252"}, {"ee", "windows-1256"},
     {"eg", "windows-1257"}, {"et", "windows-1252"}, {"fi", "windows-1252"},
     {"fr", "windows-1252"}, {"gb", "windows-1252"}, {"gr", "ISO-8859-7"},
     {"hk", "Big5"},         {"hr", "windows-1250"}, {"hu", "ISO-8859-2"},
     {"il", "windows-1255"}, {"ir", "windows-1257"}, {"is", "windows-1252"},
     {"it", "windows-1252"}, {"jp", "Shift_JIS"},    {"kr", "windows-949"},
     {"lt", "windows-1256"}, {"lv", "windows-1256"}, {"mk", "windows-1251"},
     {"nl", "windows-1252"}, {"no", "windows-1252"}, {"pl", "ISO-8859-2"},
     {"pt", "windows-1252"}, {"ro", "ISO-8859-2"},   {"rs", "windows-1251"},
     {"ru", "windows-1251"}, {"se", "windows-1252"}, {"si", "ISO-8859-2"},
     {"sk", "windows-1250"}, {"th", "windows-874"},  {"tr", "ISO-8859-9"},
     {"tw", "Big5"},         {"tz", "windows-1252"}, {"ua", "windows-1251"},
     {"us", "windows-1252"}, {"vn", "windows-1258"}, {"xa", "windows-1252"},
     {"xb", "windows-1257"}});

// The encoding of `url`'s top-level domain, or empty.
std::string GetEncodingFromDomain(const GURL& url) {
  const std::vector<std::string_view> tokens = base::SplitStringPiece(
      url.host(), ".", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  if (!tokens.empty()) {
    for (const LegacyEncoding& encoding : kEncodings) {
      if (tokens.back() == encoding.domain) {
        return CanonicalTextEncodingName(encoding.encoding);
      }
    }
  }
  return std::string();
}

struct LocaleEncoding {
  std::string_view language;
  std::string_view encoding;
};

// IDS_DEFAULT_ENCODING of each locale of Chrome's user interface whose own is
// not windows-1252, by its language: components/strings has it for each
// locale. Chinese differs between its locales, as does nothing else.
constexpr auto kLocaleEncodings = std::to_array<LocaleEncoding>({
    {"bg", "windows-1251"}, {"cs", "windows-1250"}, {"el", "ISO-8859-7"},
    {"et", "windows-1257"}, {"fa", "windows-1256"}, {"ar", "windows-1256"},
    {"he", "windows-1255"}, {"iw", "windows-1255"}, {"hr", "windows-1250"},
    {"hu", "ISO-8859-2"},   {"ja", "Shift_JIS"},    {"ko", "windows-949"},
    {"lt", "windows-1257"}, {"lv", "windows-1257"}, {"pl", "ISO-8859-2"},
    {"ro", "ISO-8859-2"},   {"ru", "windows-1251"}, {"sk", "windows-1250"},
    {"sl", "ISO-8859-2"},   {"sr", "windows-1251"}, {"th", "windows-874"},
    {"tr", "ISO-8859-9"},   {"uk", "windows-1251"}, {"vi", "windows-1258"},
});

// net::HttpUtil::IsTokenChar().
bool IsTokenChar(char c) {
  return base::IsAsciiAlphaNumeric(c) ||
         std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}

bool IsToken(std::string_view value) {
  return !value.empty() && std::ranges::all_of(value, IsTokenChar);
}

// Blink's DetermineContentType().
ContentType DetermineContentType(std::string_view mime_type) {
  if (base::EqualsCaseInsensitiveASCII(mime_type, "text/css")) {
    return ContentType::kCss;
  }
  if (base::EqualsCaseInsensitiveASCII(mime_type, "text/html")) {
    return ContentType::kHtml;
  }
  if (IsXmlMimeType(mime_type)) {
    return ContentType::kXml;
  }
  return ContentType::kPlainText;
}

}  // namespace

std::string DefaultTextEncodingForLocale(std::string_view locale) {
  const std::vector<std::string_view> subtags = base::SplitStringPiece(
      locale, "-_", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  if (subtags.empty()) {
    return std::string(kWindows1252EncodingName);
  }
  const std::string language = base::ToLowerASCII(subtags[0]);
  if (language == "zh") {
    // zh-TW's user interface for Taiwan, Hong Kong, Macao and Traditional
    // Chinese, as l10n_util picks it, and zh-CN's for the rest.
    for (size_t i = 1; i < subtags.size(); ++i) {
      for (const std::string_view traditional : {"tw", "hk", "mo", "hant"}) {
        if (base::EqualsCaseInsensitiveASCII(subtags[i], traditional)) {
          return CanonicalTextEncodingName(std::string_view("Big5"));
        }
      }
    }
    return CanonicalTextEncodingName(std::string_view("GBK"));
  }
  for (const LocaleEncoding& entry : kLocaleEncodings) {
    if (language == entry.language) {
      return CanonicalTextEncodingName(entry.encoding);
    }
  }
  return std::string(kWindows1252EncodingName);
}

bool IsXmlMimeType(std::string_view mime_type) {
  if (base::EqualsCaseInsensitiveASCII(mime_type, "text/xml") ||
      base::EqualsCaseInsensitiveASCII(mime_type, "application/xml")) {
    return true;
  }
  // An XML MIME type is of the form, as RFC 3023 and RFC 2045 have it:
  // ^[0-9a-zA-Z_\-+~!$\^{}|.%'`#&*]+/[0-9a-zA-Z_\-+~!$\^{}|.%'`#&*]+\+xml$
  const size_t length = mime_type.size();
  if (length < 7) {
    return false;
  }
  if (mime_type[0] == '/' || mime_type[length - 5] == '/' ||
      !base::EndsWith(mime_type, "+xml",
                      base::CompareCase::INSENSITIVE_ASCII)) {
    return false;
  }
  bool has_slash = false;
  for (size_t i = 0; i < length - 4; ++i) {
    const char ch = mime_type[i];
    if (base::IsAsciiAlphaNumeric(ch) ||
        std::string_view("_-+~!$^{}|.%'`#&*").find(ch) !=
            std::string_view::npos) {
      continue;
    }
    if (ch == '/' && !has_slash) {
      has_slash = true;
      continue;
    }
    return false;
  }
  return true;
}

bool IsJsonMimeType(std::string_view mime_type) {
  if (base::EqualsCaseInsensitiveASCII(mime_type, "application/json") ||
      base::EqualsCaseInsensitiveASCII(mime_type, "text/json")) {
    return true;
  }
  // net::MatchesMimeType("*+json", ...) with kWildcardSlashAndTokens.
  const size_t slash = mime_type.find('/');
  return slash != std::string_view::npos &&
         IsToken(mime_type.substr(0, slash)) &&
         IsToken(mime_type.substr(slash + 1)) &&
         base::EndsWith(mime_type, "+json",
                        base::CompareCase::INSENSITIVE_ASCII);
}

std::unique_ptr<TextResourceDecoder> BuildTextResourceDecoder(
    const GURL& url,
    std::string_view mime_type,
    std::string_view charset,
    std::string_view locale,
    const ParentFrameEncoding* parent) {
  const std::string encoding_from_domain = GetEncodingFromDomain(url);
  // A frame takes its parent's encoding only if it can access the parent, as
  // a frame of another origin could be made to look like it in the parent's
  // encoding to the detector.
  bool use_hint_encoding = parent != nullptr;

  const std::string default_encoding =
      !encoding_from_domain.empty() ? encoding_from_domain
                                    : DefaultTextEncodingForLocale(locale);
  std::unique_ptr<TextResourceDecoder> decoder;
  // A document of XML or JSON that names no encoding is UTF-8, without the
  // detector.
  if (IsXmlMimeType(mime_type)) {
    decoder = std::make_unique<TextResourceDecoder>(
        TextResourceDecoderOptions(ContentType::kXml, default_encoding));
    use_hint_encoding = false;
  } else if (IsJsonMimeType(mime_type)) {
    decoder = std::make_unique<TextResourceDecoder>(
        TextResourceDecoderOptions(ContentType::kJson, default_encoding));
    use_hint_encoding = false;
  } else {
    std::string hint_encoding;
    if (use_hint_encoding && parent->detected_heuristically) {
      hint_encoding = parent->encoding;
    }
    decoder = std::make_unique<TextResourceDecoder>(
        TextResourceDecoderOptions::CreateWithAutoDetection(
            DetermineContentType(mime_type), default_encoding, hint_encoding,
            url, locale));
  }

  if (!charset.empty()) {
    decoder->SetEncoding(
        CanonicalTextEncodingName(charset),
        TextResourceDecoder::EncodingSource::kEncodingFromHttpHeader);
  } else if (use_hint_encoding) {
    decoder->SetEncoding(
        parent->encoding,
        TextResourceDecoder::EncodingSource::kEncodingFromParentFrame);
  }
  return decoder;
}

std::unique_ptr<TextResourceDecoder> BuildClassicScriptDecoder(
    std::string_view encoding,
    std::string_view charset) {
  auto decoder = std::make_unique<TextResourceDecoder>(
      TextResourceDecoderOptions(ContentType::kPlainText, encoding));
  decoder->SetEncoding(
      CanonicalTextEncodingName(charset),
      TextResourceDecoder::EncodingSource::kEncodingFromHttpHeader);
  return decoder;
}

}  // namespace mitto
