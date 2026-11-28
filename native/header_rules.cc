#include "mitto/header_rules.h"

#include <stdint.h>

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/fixed_flat_set.h"
#include "base/containers/span.h"
#include "base/strings/strcat.h"
#include "base/strings/string_split.h"
#include "base/strings/string_tokenizer.h"
#include "base/strings/string_util.h"
#include "mitto/http_byte_range.h"

namespace mitto {
namespace {

// The Fetch Standard's forbidden request-header names, with User-Agent, which
// Chrome still keeps from scripts (crbug.com/571722).
constexpr std::array<std::string_view, 22> kForbiddenHeaderNames = {
    "accept-charset",
    "accept-encoding",
    "access-control-request-headers",
    "access-control-request-method",
    "connection",
    "content-length",
    "cookie",
    "cookie2",
    "date",
    "dnt",
    "expect",
    "host",
    "keep-alive",
    "origin",
    "referer",
    "set-cookie",
    "te",
    "trailer",
    "transfer-encoding",
    "upgrade",
    "user-agent",
    "via",
};

// A forbidden method in one of these lines makes it forbidden.
constexpr std::array<std::string_view, 3> kMethodOverrideNames = {
    "x-http-method",
    "x-http-method-override",
    "x-method-override",
};

constexpr std::array<std::string_view, 3> kForbiddenMethods = {
    "connect",
    "trace",
    "track",
};

bool IsHttpWhitespace(char c) {
  return c == '\t' || c == '\n' || c == '\r' || c == ' ';
}

// https://fetch.spec.whatwg.org/#cors-unsafe-request-header-byte
bool IsCorsUnsafeRequestHeaderByte(char c) {
  const auto u = static_cast<uint8_t>(c);
  return (u < 0x20 && u != 0x09) || u == 0x22 || u == 0x28 || u == 0x29 ||
         u == 0x3a || u == 0x3c || u == 0x3e || u == 0x3f || u == 0x40 ||
         u == 0x5b || u == 0x5c || u == 0x5d || u == 0x7b || u == 0x7d ||
         u == 0x7f;
}

bool IsSimilarToDoubleABNF(std::string_view value) {
  if (value.empty() || !base::IsAsciiDigit(value.front())) {
    return false;
  }
  bool period_found = false;
  bool digit_found_after_period = false;
  for (const char c : value) {
    if (base::IsAsciiDigit(c)) {
      digit_found_after_period |= period_found;
      continue;
    }
    if (c != '.' || period_found) {
      return false;
    }
    period_found = true;
  }
  return !period_found || digit_found_after_period;
}

bool IsSimilarToIntABNF(std::string_view value) {
  return !value.empty() && std::ranges::all_of(value, base::IsAsciiDigit<char>);
}

}  // namespace

bool IsHttpToken(std::string_view text) {
  if (text.empty()) {
    return false;
  }
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte >= 0x7F || byte <= 0x20 || c == '(' || c == ')' || c == '<' ||
        c == '>' || c == '@' || c == ',' || c == ';' || c == ':' || c == '\\' ||
        c == '"' || c == '/' || c == '[' || c == ']' || c == '?' || c == '=' ||
        c == '{' || c == '}') {
      return false;
    }
  }
  return true;
}

bool IsValidHeaderValue(std::string_view value) {
  return value.find_first_of(std::string_view("\0\r\n", 3)) ==
         std::string_view::npos;
}

std::string_view NormalizeHeaderValue(std::string_view value) {
  while (!value.empty() && IsHttpWhitespace(value.front())) {
    value.remove_prefix(1);
  }
  while (!value.empty() && IsHttpWhitespace(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

bool IsForbiddenRequestHeader(std::string_view name, std::string_view value) {
  if (base::StartsWith(name, "proxy-", base::CompareCase::INSENSITIVE_ASCII) ||
      base::StartsWith(name, "sec-", base::CompareCase::INSENSITIVE_ASCII)) {
    return true;
  }
  for (const std::string_view forbidden : kForbiddenHeaderNames) {
    if (base::EqualsCaseInsensitiveASCII(name, forbidden)) {
      return true;
    }
  }
  for (const std::string_view override_name : kMethodOverrideNames) {
    if (!base::EqualsCaseInsensitiveASCII(name, override_name)) {
      continue;
    }
    const std::string_view methods_value = NormalizeHeaderValue(value);
    base::StringViewTokenizer methods(methods_value, ",");
    while (methods.GetNext()) {
      const std::string_view method =
          base::TrimWhitespaceASCII(methods.token_piece(), base::TRIM_ALL);
      for (const std::string_view forbidden : kForbiddenMethods) {
        if (base::EqualsCaseInsensitiveASCII(method, forbidden)) {
          return true;
        }
      }
    }
  }
  return false;
}

bool IsForbiddenResponseHeaderName(std::string_view name) {
  return base::EqualsCaseInsensitiveASCII(name, "set-cookie") ||
         base::EqualsCaseInsensitiveASCII(name, "set-cookie2");
}

bool IsForbiddenMethod(std::string_view method) {
  for (const std::string_view forbidden : kForbiddenMethods) {
    if (base::EqualsCaseInsensitiveASCII(method, forbidden)) {
      return true;
    }
  }
  return false;
}

bool IsCorsSafelistedMethod(std::string_view method) {
  return method == "GET" || method == "HEAD" || method == "POST";
}

bool IsNoCorsSafelistedHeaderName(std::string_view name) {
  return base::EqualsCaseInsensitiveASCII(name, "accept") ||
         base::EqualsCaseInsensitiveASCII(name, "accept-language") ||
         base::EqualsCaseInsensitiveASCII(name, "content-language") ||
         base::EqualsCaseInsensitiveASCII(name, "content-type");
}

bool IsPrivilegedNoCorsHeaderName(std::string_view name) {
  return base::EqualsCaseInsensitiveASCII(name, "range");
}

bool IsNoCorsSafelistedHeader(std::string_view name, std::string_view value) {
  if (!IsNoCorsSafelistedHeaderName(name) || value.size() > 128) {
    return false;
  }
  if (base::EqualsCaseInsensitiveASCII(name, "accept")) {
    return !std::ranges::any_of(value, IsCorsUnsafeRequestHeaderByte);
  }
  if (base::EqualsCaseInsensitiveASCII(name, "content-type")) {
    return IsCorsSafelistedContentType(value);
  }
  // Accept-Language and Content-Language.
  return std::ranges::all_of(value, [](char c) {
    return base::IsAsciiAlphaNumeric(c) || c == ' ' || c == '*' || c == ',' ||
           c == '-' || c == '.' || c == ';' || c == '=';
  });
}

bool IsCorsSafelistedContentType(std::string_view media_type) {
  if (std::ranges::any_of(media_type, IsCorsUnsafeRequestHeaderByte)) {
    return false;
  }
  const std::optional<std::string> mime_type =
      ExtractMimeTypeFromMediaType(base::ToLowerASCII(media_type),
                                   /*accept_comma_separated=*/false);
  return mime_type &&
         (*mime_type == "application/x-www-form-urlencoded" ||
          *mime_type == "multipart/form-data" || *mime_type == "text/plain");
}

bool IsCorsSafelistedHeader(std::string_view name, std::string_view value) {
  if (value.size() > 128) {
    return false;
  }
  static constexpr auto kSafeNames = base::MakeFixedFlatSet<std::string_view>({
      "accept",
      "accept-language",
      "content-language",
      "content-type",
      "range",
      "device-memory",
      "dpr",
      "width",
      "viewport-width",
      "rtt",
      "downlink",
      "ect",
      "sec-ch-ua",
      "sec-ch-ua-arch",
      "sec-ch-ua-platform",
      "sec-ch-ua-model",
      "sec-ch-ua-mobile",
      "sec-ch-ua-full-version",
      "sec-ch-ua-platform-version",
      "sec-ch-prefers-color-scheme",
      "sec-ch-ua-bitness",
      "sec-ch-viewport-height",
      "sec-ch-device-memory",
      "sec-ch-dpr",
      "sec-ch-width",
      "sec-ch-viewport-width",
      "sec-ch-ua-full-version-list",
      "sec-ch-ua-wow64",
      "save-data",
      "sec-ch-prefers-reduced-motion",
      "sec-ch-ua-form-factors",
      "sec-ch-prefers-reduced-transparency",
  });
  const std::string lower_name = base::ToLowerASCII(name);
  if (!kSafeNames.contains(lower_name)) {
    return false;
  }
  if (IsNoCorsSafelistedHeaderName(lower_name)) {
    return IsNoCorsSafelistedHeader(lower_name, value);
  }
  if (lower_name == "range") {
    const std::optional<HttpByteRange> range =
        ParseFetchSingleRange(value, /*allow_whitespace=*/false);
    return range && !range->IsSuffixByteRange() && range->IsValid() &&
           range->first_byte_position() !=
               std::numeric_limits<int64_t>::max() &&
           range->last_byte_position() != std::numeric_limits<int64_t>::max();
  }
  if (lower_name == "device-memory" || lower_name == "dpr" ||
      lower_name == "downlink") {
    return IsSimilarToDoubleABNF(value);
  }
  if (lower_name == "width" || lower_name == "viewport-width" ||
      lower_name == "rtt") {
    return IsSimilarToIntABNF(value);
  }
  if (lower_name == "ect") {
    return value == "slow-2g" || value == "2g" || value == "3g" ||
           value == "4g";
  }
  if (lower_name == "save-data") {
    return base::ToLowerASCII(value) == "on";
  }
  return true;
}

bool ContainsOnlyCorsSafelistedHeaders(
    base::span<const std::pair<std::string, std::string>> headers) {
  constexpr size_t kSafeListValueSizeMax = 1024;
  size_t safe_list_value_size = 0;
  for (const auto& [name, value] : headers) {
    if (!IsCorsSafelistedHeader(name, value)) {
      return false;
    }
    safe_list_value_size += value.size();
  }
  return safe_list_value_size <= kSafeListValueSizeMax;
}

bool IsCorsSafelistedResponseHeaderName(std::string_view name) {
  constexpr std::array<std::string_view, 7> kNames = {
      "cache-control", "content-language", "content-length", "content-type",
      "expires",       "last-modified",    "pragma",
  };
  for (const std::string_view safelisted : kNames) {
    if (base::EqualsCaseInsensitiveASCII(name, safelisted)) {
      return true;
    }
  }
  return false;
}

bool HasLocation(std::string_view headers) {
  for (const std::string_view line : base::SplitStringPiece(
           headers, "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    const size_t colon = line.find(": ");
    if (colon != std::string_view::npos && colon + 2 < line.size() &&
        base::EqualsCaseInsensitiveASCII(line.substr(0, colon), "location")) {
      return true;
    }
  }
  return false;
}

bool IsConditionalHeader(std::string_view name) {
  static constexpr std::array<std::string_view, 5> kConditional = {
      "if-match", "if-modified-since", "if-none-match", "if-range",
      "if-unmodified-since"};
  return std::ranges::any_of(
      kConditional, [name](std::string_view conditional) {
        return base::EqualsCaseInsensitiveASCII(name, conditional);
      });
}

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

// Whether a response's Cache-Control value, `raw_value`, has the directive
// no-store, as Blink's ParseCacheControlDirectives() finds it, with its
// separators of RFC 2616.
// Upstream: third_party/blink/renderer/platform/network/http_parsers.cc
bool CacheControlContainsNoStore(std::string_view raw_value) {
  constexpr std::string_view kSeparators = "()<>@,;:\\\"/[]?={} \t";
  std::string value;
  for (const char c : raw_value) {
    if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F) {
      value.push_back(c);
    }
  }
  const auto strip = [](std::string_view text) {
    return base::TrimString(text, " ", base::TRIM_ALL);
  };
  const auto trim_to_separator = [&](std::string_view text) {
    return text.substr(0, text.find_first_of(kSeparators));
  };
  bool no_store = false;
  const auto add = [&](std::string_view directive) {
    if (base::EqualsCaseInsensitiveASCII(directive, "no-store")) {
      no_store = true;
    }
  };
  const std::string_view header = value;
  const size_t max = header.size();
  for (size_t pos = 0; pos < max;) {
    const size_t next_comma = header.find(',', pos);
    const size_t next_equal = header.find('=', pos);
    if (next_equal != std::string_view::npos &&
        (next_equal < next_comma || next_comma == std::string_view::npos)) {
      // A directive with a value.
      add(trim_to_separator(strip(header.substr(pos, next_equal - pos))));
      pos = next_equal + 1;
      const std::string_view rest = strip(header.substr(pos));
      if (rest.starts_with('"')) {
        const size_t next_quote = rest.find('"', 1);
        if (next_quote == std::string_view::npos) {
          return no_store;
        }
        pos = header.find('"', pos) + next_quote + 1;
        const size_t comma = header.find(',', pos);
        if (comma == std::string_view::npos) {
          return no_store;
        }
        pos = comma + 1;
      } else {
        if (rest.find(',') == std::string_view::npos) {
          return no_store;
        }
        pos = header.find(',', pos) + 1;
      }
    } else if (next_comma != std::string_view::npos &&
               (next_comma < next_equal ||
                next_equal == std::string_view::npos)) {
      add(trim_to_separator(strip(header.substr(pos, next_comma - pos))));
      pos = next_comma + 1;
    } else {
      add(trim_to_separator(strip(header.substr(pos))));
      return no_store;
    }
  }
  return no_store;
}

std::set<std::string> CorsExposedHeaderNames(std::string_view field) {
  // HTTPHeaderNameListParser: one element that is not a name, and not empty,
  // leaves the list with no name.
  std::set<std::string> names;
  for (const std::string_view element : base::SplitStringPiece(
           field, ",", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL)) {
    const std::string_view name =
        base::TrimString(element, " \t", base::TRIM_ALL);
    if (name.empty()) {
      continue;
    }
    if (!IsHttpToken(name)) {
      return {};
    }
    names.insert(base::ToLowerASCII(name));
  }
  return names;
}

std::optional<std::string> ExtractMimeTypeFromMediaType(
    std::string_view media_type,
    bool accept_comma_separated) {
  size_t end = media_type.find(';');
  if (accept_comma_separated) {
    end = std::min(end, media_type.find(','));
  }
  media_type = media_type.substr(0, end);
  const size_t slash = media_type.find('/');
  if (slash == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view type = base::TrimWhitespaceASCII(
      media_type.substr(0, slash), base::TRIM_LEADING);
  const std::string_view subtype = base::TrimWhitespaceASCII(
      media_type.substr(slash + 1), base::TRIM_TRAILING);
  if (!IsHttpToken(type) || !IsHttpToken(subtype)) {
    return std::nullopt;
  }
  return base::StrCat({type, "/", subtype});
}

}  // namespace mitto
