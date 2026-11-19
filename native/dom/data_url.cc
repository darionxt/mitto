#include "mitto/dom/data_url.h"

#include <algorithm>
#include <string_view>
#include <vector>

#include "base/base64.h"
#include "base/strings/escape.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace mitto {
namespace {

constexpr std::string_view kBase64Tag = "base64";
constexpr std::string_view kCharsetTag = "charset=";

// HttpUtil::IsTokenChar(), RFC 7230's tchar.
bool IsTokenChar(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return !(u >= 0x7F || u <= 0x20 || c == '(' || c == ')' || c == '<' ||
           c == '>' || c == '@' || c == ',' || c == ';' || c == ':' ||
           c == '\\' || c == '"' || c == '/' || c == '[' || c == ']' ||
           c == '?' || c == '=' || c == '{' || c == '}');
}

bool IsToken(std::string_view text) {
  return !text.empty() && std::ranges::all_of(text, IsTokenChar);
}

// MimeUtil::ParseMimeTypeWithoutParameter().
bool IsMimeTypeWithoutParameter(std::string_view type) {
  const std::vector<std::string_view> components = base::SplitStringPiece(
      type, "/", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  return components.size() == 2 &&
         IsToken(
             base::TrimWhitespaceASCII(components[0], base::TRIM_LEADING)) &&
         IsToken(base::TrimWhitespaceASCII(components[1], base::TRIM_TRAILING));
}

// ParseCharsetParameter(): the value after "charset=", the first valid one
// kept, and one that starts with a space kept quoted.
bool ParseCharsetParameter(std::string_view candidate,
                           std::string& charset,
                           std::string_view& quoted_charset) {
  if (candidate.empty()) {
    return false;
  }
  if (candidate.size() >= 2 && candidate.front() == '"' &&
      candidate.back() == '"') {
    const std::string_view unquoted = candidate.substr(1, candidate.size() - 2);
    if (!unquoted.empty() && unquoted[0] == ' ') {
      quoted_charset = unquoted;
      return true;
    }
    if (!IsToken(unquoted)) {
      return false;
    }
    if (charset.empty()) {
      charset = std::string(unquoted);
    }
    return true;
  }
  if (candidate[0] == ' ') {
    quoted_charset = candidate;
    return true;
  }
  if (!IsToken(candidate)) {
    return false;
  }
  if (charset.empty()) {
    charset = std::string(candidate);
  }
  return true;
}

// ParseMetadataWithParameterPreservation(), as
// kDataUrlMimeTypeParameterPreservation has it, on by default.
bool ParseMetadata(const std::vector<std::string_view>& meta_data,
                   std::string& mime_type,
                   std::string& charset,
                   bool& base64_encoded) {
  struct Parameter {
    std::string_view value;
    bool needs_closing_quote = false;
  };
  std::vector<Parameter> parameters;
  std::string_view quoted_charset;
  const bool mime_type_is_empty = mime_type.empty();
  auto iter = meta_data.cbegin();
  if (iter != meta_data.cend()) {
    ++iter;
  }
  // "base64" counts only as the last token before the comma.
  if (iter != meta_data.cend() &&
      base::EqualsCaseInsensitiveASCII(meta_data.back(), kBase64Tag)) {
    base64_encoded = true;
  }
  const auto end_iter =
      base64_encoded ? meta_data.cend() - 1 : meta_data.cend();
  for (; iter < end_iter; ++iter) {
    const size_t equals = iter->find('=');
    if (equals == std::string_view::npos) {
      continue;
    }
    if (base::StartsWith(*iter, kCharsetTag,
                         base::CompareCase::INSENSITIVE_ASCII)) {
      const std::string_view candidate = iter->substr(kCharsetTag.size());
      if (!ParseCharsetParameter(candidate, charset, quoted_charset) &&
          !candidate.empty()) {
        return false;
      }
      continue;
    }
    const std::string_view name = iter->substr(0, equals);
    if (name.empty() || name.back() == ' ') {
      continue;
    }
    const std::string_view value = iter->substr(equals + 1);
    const bool starts_with_quote = !value.empty() && value.front() == '"';
    const bool ends_with_quote = value.size() >= 2 && value.back() == '"';
    parameters.push_back({*iter, starts_with_quote && !ends_with_quote});
  }
  if (mime_type_is_empty) {
    mime_type = "text/plain";
    if (charset.empty() && parameters.empty() &&
        (meta_data.size() <= 1 || (meta_data.size() == 2 && base64_encoded))) {
      charset = "US-ASCII";
    }
  } else if (!IsMimeTypeWithoutParameter(mime_type)) {
    mime_type = "text/plain";
    charset = "US-ASCII";
    parameters.clear();
  }
  for (const Parameter& parameter : parameters) {
    mime_type.append(";");
    mime_type.append(parameter.value);
    if (parameter.needs_closing_quote) {
      mime_type.append("\"");
    }
  }
  if (!quoted_charset.empty()) {
    mime_type.append(";charset=\"").append(quoted_charset).append("\"");
  }
  return true;
}

}  // namespace

std::optional<DataUrlResponse> ParseDataUrl(const GURL& url) {
  if (!url.is_valid() || !url.SchemeIs(url::kDataScheme)) {
    return std::nullopt;
  }
  const std::string_view content = url.GetContentPiece();
  const size_t comma = content.find(',');
  if (comma == std::string_view::npos) {
    return std::nullopt;
  }
  const std::vector<std::string_view> meta_data =
      base::SplitStringPiece(content.substr(0, comma), ";",
                             base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
  std::string mime_type;
  if (!meta_data.empty()) {
    mime_type = base::ToLowerASCII(meta_data[0]);
  }
  std::string charset;
  bool base64_encoded = false;
  if (!ParseMetadata(meta_data, mime_type, charset, base64_encoded)) {
    return std::nullopt;
  }
  DataUrlResponse response;
  const std::string_view raw_body = content.substr(comma + 1);
  if (base64_encoded) {
    // kFurtherOptimizeParsingDataUrls: forgiving base64, percent-decoded
    // first if it does not decode as it is.
    if (!base::Base64Decode(raw_body, &response.body,
                            base::Base64DecodePolicy::kForgiving)) {
      if (!base::Base64Decode(base::UnescapeBinaryURLComponent(raw_body),
                              &response.body,
                              base::Base64DecodePolicy::kForgiving)) {
        return std::nullopt;
      }
    }
  } else {
    response.body = base::UnescapeBinaryURLComponent(raw_body);
  }
  // BuildResponse(): the Content-Type line, then the essence alone, which a
  // type with no "/" lacks.
  response.content_type = mime_type;
  if (!charset.empty()) {
    response.content_type += ";charset=" + charset;
  }
  const size_t parameters = mime_type.find(';');
  const std::string essence = std::string(base::TrimWhitespaceASCII(
      std::string_view(mime_type).substr(0, parameters), base::TRIM_ALL));
  if (essence.find('/') == std::string::npos) {
    return std::nullopt;
  }
  response.mime_type = base::ToLowerASCII(essence);
  response.charset = std::move(charset);
  return response;
}

}  // namespace mitto
