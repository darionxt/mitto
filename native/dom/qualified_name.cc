#include "mitto/dom/qualified_name.h"

#include <utility>

#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/dom/html_names.h"

namespace mitto {

namespace {

constexpr char kInvalidCharacterError[] = "InvalidCharacterError";

bool IsAsciiAlpha(char16_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// ASCII whitespace as the Infra Standard has it, Blink's IsAsciiSpaceWhatwg().
bool IsAsciiWhitespace(char16_t c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

// Blink's ParseNamespacePrefix(): the first character that a valid namespace
// prefix cannot hold, or nothing.
std::optional<char16_t> InvalidPrefixCharacter(std::u16string_view prefix) {
  for (const char16_t c : prefix) {
    if (c == '>' || c == '/' || c == 0 || IsAsciiWhitespace(c)) {
      return c;
    }
  }
  return std::nullopt;
}

// Blink's ParseAttributeLocalName().
std::optional<char16_t> InvalidAttributeCharacter(std::u16string_view name) {
  for (const char16_t c : name) {
    if (c == 0 || IsAsciiWhitespace(c) || c == '/' || c == '=' || c == '>') {
      return c;
    }
  }
  return std::nullopt;
}

// Blink's ParseElementLocalName(), for a name that is not empty.
std::optional<char16_t> InvalidElementCharacter(std::u16string_view name) {
  if (IsAsciiAlpha(name[0])) {
    for (const char16_t c : name.substr(1)) {
      if (c == 0 || IsAsciiWhitespace(c) || c == '/' || c == '>') {
        return c;
      }
    }
    return std::nullopt;
  }
  if (name[0] < 0x80 && name[0] != ':' && name[0] != '_') {
    return name[0];
  }
  for (const char16_t c : name.substr(1)) {
    if (!IsAsciiAlpha(c) && !(c >= '0' && c <= '9') && c != '-' && c != '.' &&
        c != ':' && c != '_' && c < 0x80) {
      return c;
    }
  }
  return std::nullopt;
}

void ThrowInvalidQualifiedName(const std::u16string& qualified_name,
                               std::string_view problem,
                               bindings::ExceptionState& exception_state) {
  exception_state.ThrowDOMException(
      kInvalidCharacterError,
      base::StrCat({"The qualified name provided ('",
                    base::UTF16ToUTF8(qualified_name), "') ", problem}));
}

}  // namespace

bool IsValidElementLocalName(std::u16string_view name) {
  return !name.empty() && !InvalidElementCharacter(name);
}

bool IsValidAttributeLocalName(std::u16string_view name) {
  return !name.empty() && !InvalidAttributeCharacter(name);
}

// static
std::optional<QualifiedName> QualifiedName::Parse(
    const std::optional<std::u16string>& namespace_uri,
    const std::u16string& qualified_name,
    ParsingMode mode,
    bindings::ExceptionState& exception_state) {
  if (qualified_name.empty()) {
    exception_state.ThrowDOMException(kInvalidCharacterError,
                                      "The qualified name provided is empty.");
    return std::nullopt;
  }
  // The DOM standard splits at the first colon. Blink, without the
  // experimental SplitQualifiedNameOnFirstColon, takes the local name from
  // between the first two.
  const std::u16string_view name = qualified_name;
  const size_t colon = name.find(':');
  std::optional<std::u16string> prefix;
  std::u16string_view local_name = name;
  if (colon != std::u16string_view::npos) {
    const std::u16string_view prefix_part = name.substr(0, colon);
    local_name = name.substr(colon + 1);
    local_name = local_name.substr(0, local_name.find(':'));
    if (prefix_part.empty()) {
      ThrowInvalidQualifiedName(
          qualified_name, "has an empty namespace prefix.", exception_state);
      return std::nullopt;
    }
    if (const std::optional<char16_t> c = InvalidPrefixCharacter(prefix_part)) {
      ThrowInvalidQualifiedName(
          qualified_name,
          base::StrCat({"contains the invalid character '",
                        base::UTF16ToUTF8(std::u16string(1, *c)), "'."}),
          exception_state);
      return std::nullopt;
    }
    prefix = std::u16string(prefix_part);
  }
  if (local_name.empty()) {
    ThrowInvalidQualifiedName(qualified_name, "has an empty local name.",
                              exception_state);
    return std::nullopt;
  }
  const std::optional<char16_t> invalid =
      mode == ParsingMode::kAttribute ? InvalidAttributeCharacter(local_name)
                                      : InvalidElementCharacter(local_name);
  if (invalid) {
    ThrowInvalidQualifiedName(
        qualified_name,
        base::StrCat({"contains the invalid character '",
                      base::UTF16ToUTF8(std::u16string(1, *invalid)), "'."}),
        exception_state);
    return std::nullopt;
  }
  return QualifiedName{
      .prefix = std::move(prefix),
      .local_name = std::u16string(local_name),
      .namespace_uri = namespace_uri && !namespace_uri->empty() ? namespace_uri
                                                                : std::nullopt,
  };
}

bool QualifiedName::HasValidNamespace() const {
  if (prefix && !namespace_uri) {
    return false;
  }
  if (prefix == u"xml" && namespace_uri != html_names::kXmlNamespace) {
    return false;
  }
  if (prefix == u"xmlns" || (!prefix && local_name == u"xmlns")) {
    return namespace_uri == html_names::kXmlnsNamespace;
  }
  return namespace_uri != html_names::kXmlnsNamespace;
}

std::u16string QualifiedName::ToString() const {
  return prefix ? base::StrCat({*prefix, u":", local_name}) : local_name;
}

}  // namespace mitto
