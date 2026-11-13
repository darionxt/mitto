#include "mitto/encoding/content_type.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "base/strings/string_tokenizer.h"
#include "base/strings/string_util.h"

namespace mitto {

namespace {

// HTTP's linear white space, as //net's HTTP_LWS.
constexpr std::string_view kLws = " \t";

bool IsLws(char c) {
  return kLws.find(c) != std::string_view::npos;
}

// HttpUtil::TrimLWS().
std::string_view TrimLws(std::string_view value) {
  return base::TrimString(value, kLws, base::TRIM_ALL);
}

// net::ParseMimeType(): the type of `type_str`, without white space, and its
// parameters, or false if it has no slash.
bool ParseMimeType(std::string_view type_str,
                   std::string& mime_type,
                   std::vector<std::pair<std::string, std::string>>& params) {
  // White space around the type goes, and so does a comment in parentheses
  // after it, which some servers send.
  const size_t type_val =
      std::min(type_str.find_first_not_of(kLws), type_str.size());
  size_t type_end = type_str.find_first_of(" \t;(", type_val);
  if (type_end == std::string_view::npos) {
    type_end = type_str.size();
  }
  const size_t slash_pos = type_str.find('/');
  if (slash_pos == std::string_view::npos || slash_pos > type_end) {
    return false;
  }
  mime_type = std::string(type_str.substr(type_val, type_end - type_val));

  // Semicolons may be in quoted strings, so the parameters cannot be split
  // around them first. Much as the MIME Sniffing Standard has it, but for
  // what characters may be in them and the spaces after "=", which go.
  params.clear();
  size_t offset = type_str.find(';', type_end);
  while (offset < type_str.size()) {
    ++offset;
    offset = type_str.find_first_not_of(kLws, offset);
    const size_t param_name_start = offset;
    // A name runs to a semicolon or an equals sign, its trailing spaces too.
    offset = type_str.find_first_of(";=", offset);
    // A name with no value is not a parameter.
    if (offset == std::string_view::npos || type_str[offset] == ';') {
      continue;
    }
    std::string param_name(
        type_str.substr(param_name_start, offset - param_name_start));
    ++offset;
    offset = type_str.find_first_not_of(kLws, offset);
    std::string param_value;
    if (offset == std::string_view::npos || type_str[offset] == ';') {
      // A value of only white space is skipped.
      continue;
    } else if (type_str[offset] != '"') {
      const size_t value_start = offset;
      offset = type_str.find(';', offset);
      size_t value_end =
          offset == std::string_view::npos ? type_str.size() : offset;
      while (value_end > value_start && IsLws(type_str[value_end - 1])) {
        --value_end;
      }
      param_value =
          std::string(type_str.substr(value_start, value_end - value_start));
    } else {
      // A quoted string, with backslashes escaping what follows them, up to
      // its closing quote, white space and all.
      ++offset;
      while (offset < type_str.size() && type_str[offset] != '"') {
        if (type_str[offset] == '\\' && offset + 1 < type_str.size()) {
          ++offset;
        }
        param_value += type_str[offset];
        ++offset;
      }
      offset = type_str.find(';', offset);
    }
    params.emplace_back(std::move(param_name), std::move(param_value));
  }
  return true;
}

// HttpUtil::ParseContentType(), without the boundary.
void ParseContentType(std::string_view content_type_str,
                      std::string& mime_type,
                      std::string& charset,
                      bool& had_charset) {
  std::string mime_type_value;
  std::vector<std::pair<std::string, std::string>> params;
  // "*/*" says nothing, and a type without a slash is junk, such as what some
  // servers send after the charset, commas and all.
  if (!ParseMimeType(content_type_str, mime_type_value, params) ||
      content_type_str == "*/*") {
    return;
  }
  std::string charset_value;
  bool type_has_charset = false;
  for (const auto& [name, value] : params) {
    if (!type_has_charset &&
        base::EqualsCaseInsensitiveASCII(name, "charset")) {
      type_has_charset = true;
      charset_value = std::string(TrimLws(value));
    }
  }
  // The same type again only changes the charset, and keeps the one there is
  // if it names none.
  const bool eq = base::EqualsCaseInsensitiveASCII(mime_type_value, mime_type);
  if (!eq) {
    mime_type = base::ToLowerASCII(mime_type_value);
  }
  if ((!eq && had_charset) || type_has_charset) {
    had_charset = true;
    charset = base::ToLowerASCII(charset_value);
  }
}

}  // namespace

MimeTypeAndCharset GetMimeTypeAndCharset(std::string_view content_type) {
  MimeTypeAndCharset result;
  bool had_charset = false;
  // HttpUtil::ValuesIterator: the values between commas outside quotes,
  // trimmed, and none that is empty.
  base::StringViewTokenizer tokenizer(content_type, ",");
  tokenizer.set_quote_chars("\"");
  while (tokenizer.GetNext()) {
    const std::string_view value = TrimLws(tokenizer.token_piece());
    if (value.empty()) {
      continue;
    }
    ParseContentType(value, result.mime_type, result.charset, had_charset);
  }
  return result;
}

}  // namespace mitto
