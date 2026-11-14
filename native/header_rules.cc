#include "mitto/header_rules.h"

#include <stdint.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <vector>

#include "base/strings/strcat.h"
#include "base/strings/string_split.h"
#include "base/strings/string_tokenizer.h"
#include "base/strings/string_util.h"

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

// net::ExtractMimeTypeFromMediaType(), without accepting a comma-separated
// list: the type and subtype before any parameters, if both are tokens.
// Upstream: net/base/mime_util.cc
std::optional<std::string> ExtractMimeType(std::string_view media_type) {
  const std::vector<std::string_view> components =
      base::SplitStringPiece(media_type.substr(0, media_type.find(';')), "/",
                             base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  if (components.size() != 2) {
    return std::nullopt;
  }
  const std::string_view type =
      base::TrimWhitespaceASCII(components[0], base::TRIM_LEADING);
  const std::string_view subtype =
      base::TrimWhitespaceASCII(components[1], base::TRIM_TRAILING);
  if (!IsHttpToken(type) || !IsHttpToken(subtype)) {
    return std::nullopt;
  }
  return base::StrCat({type, "/", subtype});
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
    if (std::ranges::any_of(value, IsCorsUnsafeRequestHeaderByte)) {
      return false;
    }
    const std::optional<std::string> mime_type =
        ExtractMimeType(base::ToLowerASCII(value));
    return mime_type &&
           (*mime_type == "application/x-www-form-urlencoded" ||
            *mime_type == "multipart/form-data" || *mime_type == "text/plain");
  }
  // Accept-Language and Content-Language.
  return std::ranges::all_of(value, [](char c) {
    return base::IsAsciiAlphaNumeric(c) || c == ' ' || c == '*' || c == ',' ||
           c == '-' || c == '.' || c == ';' || c == '=';
  });
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

}  // namespace mitto
