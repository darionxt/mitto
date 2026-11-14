#include "mitto/dom/resolved_url.h"

#include <algorithm>
#include <array>

#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "url/url_util.h"

namespace mitto {

namespace {

// The WHATWG URL Standard's special schemes, Blink's
// SchemeRegistry::IsSpecialScheme().
bool IsSpecialScheme(std::u16string_view scheme) {
  constexpr auto kSpecial = std::to_array<std::u16string_view>(
      {u"ftp", u"file", u"http", u"https", u"ws", u"wss"});
  return std::find(kSpecial.begin(), kSpecial.end(), scheme) != kSpecial.end();
}

bool IsAsciiTabOrNewline(char16_t c) {
  return c == '\t' || c == '\r' || c == '\n';
}

// Blink's RemoveUrlWhitespace().
std::u16string RemoveUrlWhitespace(std::u16string_view input) {
  std::u16string result;
  for (const char16_t c : input) {
    if (!IsAsciiTabOrNewline(c)) {
      result += c;
    }
  }
  return result;
}

// Blink's ParsePortFromString(): the leading digits without their leading
// zeros, "008080junk" giving "8080" and "000" giving "0".
std::u16string_view ParsePortFromString(std::u16string_view value) {
  size_t digits = 0;
  while (digits < value.size() && value[digits] >= '0' &&
         value[digits] <= '9') {
    ++digits;
  }
  size_t zeros = 0;
  while (zeros < digits && value[zeros] == '0') {
    ++zeros;
  }
  if (zeros == digits && zeros) {
    --zeros;
  }
  return value.substr(zeros, digits - zeros);
}

// Blink's FindHostPart(): the host before a '/', '?' or '#', and a '\' too
// in a special URL.
std::u16string_view FindHostPart(std::u16string_view host, bool is_special) {
  for (size_t i = 0; i < host.size(); ++i) {
    const char16_t c = host[i];
    if (c == '/' || c == '?' || c == '#' || (is_special && c == '\\')) {
      return host.substr(0, i);
    }
  }
  return host;
}

}  // namespace

bool IsDefaultPortForProtocol(uint16_t port, std::u16string_view protocol) {
  if (protocol.empty()) {
    return false;
  }
  if (protocol == u"http" || protocol == u"ws") {
    return port == 80;
  }
  if (protocol == u"https" || protocol == u"wss") {
    return port == 443;
  }
  if (protocol == u"ftp") {
    return port == 21;
  }
  return false;
}

ResolvedURL::ResolvedURL() = default;

ResolvedURL::ResolvedURL(const ResolvedURL& base, std::u16string_view relative)
    : is_null_(false) {
  url::RawCanonOutputT<char> output;
  is_valid_ = url::ResolveRelative(base.string_, base.parsed_, relative,
                                   nullptr, &output, &parsed_);
  string_ = std::string(output.view());
}

ResolvedURL::ResolvedURL(std::u16string_view url)
    : ResolvedURL(ResolvedURL(), url) {}

std::u16string ResolvedURL::GetString() const {
  return base::UTF8ToUTF16(string_);
}

std::u16string ResolvedURL::Component(const url::Component& component) const {
  if (component.len <= 0) {
    return std::u16string();
  }
  return base::UTF8ToUTF16(
      std::string_view(string_).substr(static_cast<size_t>(component.begin),
                                       static_cast<size_t>(component.len)));
}

std::u16string ResolvedURL::Protocol() const {
  return Component(parsed_.scheme);
}

std::u16string ResolvedURL::Host() const {
  return Component(parsed_.host);
}

int ResolvedURL::HostEnd() const {
  return parsed_.CountCharactersBefore(url::Parsed::PORT, true);
}

int ResolvedURL::PathStart() const {
  return parsed_.CountCharactersBefore(url::Parsed::PATH, false);
}

bool ResolvedURL::HasPort() const {
  return HostEnd() < PathStart();
}

uint16_t ResolvedURL::Port() const {
  if (!is_valid_ || parsed_.port.is_empty()) {
    return 0;
  }
  const int port = url::ParsePort(string_, parsed_.port);
  return port < 0 ? 0 : static_cast<uint16_t>(port);
}

std::u16string ResolvedURL::User() const {
  return Component(parsed_.username);
}

std::u16string ResolvedURL::Pass() const {
  return Component(parsed_.password);
}

std::u16string ResolvedURL::Path() const {
  return Component(parsed_.path);
}

std::u16string ResolvedURL::QueryWithLeadingQuestionMark() const {
  if (!parsed_.query.is_valid() || !is_valid_ || parsed_.query.is_empty()) {
    return std::u16string();
  }
  return u"?" + Component(parsed_.query);
}

std::u16string ResolvedURL::FragmentIdentifierWithLeadingNumberSign() const {
  if (!parsed_.ref.is_valid() || !is_valid_ || parsed_.ref.is_empty()) {
    return std::u16string();
  }
  return u"#" + Component(parsed_.ref);
}

bool ResolvedURL::IsStandard() const {
  if (is_null_ || parsed_.scheme.is_empty()) {
    return false;
  }
  return url::IsStandard(std::string_view(string_).substr(
      static_cast<size_t>(parsed_.scheme.begin),
      static_cast<size_t>(parsed_.scheme.len)));
}

bool ResolvedURL::IsHierarchical() const {
  return IsStandard() || (is_valid_ && !parsed_.has_opaque_path);
}

bool ResolvedURL::CanRemoveHost() const {
  const bool includes_credentials =
      parsed_.username.is_nonempty() || parsed_.password.is_nonempty();
  return IsHierarchical() && !includes_credentials && !HasPort();
}

void ResolvedURL::ReplaceComponents(const url::Replacements<char>& replacements,
                                    bool preserve_validity) {
  url::RawCanonOutputT<char> output;
  url::Parsed new_parsed;
  const bool valid = url::ReplaceComponents(string_, parsed_, replacements,
                                            nullptr, &output, &new_parsed);
  if (valid || !preserve_validity) {
    is_valid_ = valid;
    parsed_ = new_parsed;
    string_ = std::string(output.view());
  }
}

bool ResolvedURL::SetProtocol(std::u16string_view protocol) {
  // Blink's KURL::SetProtocol(): what comes before the first ':', which must
  // be a scheme, and a special URL keeps a special scheme.
  const std::string new_protocol =
      base::UTF16ToUTF8(protocol.substr(0, protocol.find(':')));
  url::RawCanonOutputT<char> canonical;
  url::Component component;
  if (!url::CanonicalizeScheme(new_protocol, &canonical, &component) ||
      component.is_empty()) {
    return false;
  }
  const std::u16string old_protocol = Protocol();
  if (IsSpecialScheme(old_protocol)) {
    const std::u16string canonical_protocol =
        base::UTF8ToUTF16(std::string_view(canonical.view())
                              .substr(0, static_cast<size_t>(component.len)));
    if (!IsSpecialScheme(canonical_protocol)) {
      return true;
    }
    const bool new_is_file = canonical_protocol == u"file";
    const bool old_is_file = old_protocol == u"file";
    if (new_is_file && !old_is_file &&
        (HasPort() || parsed_.username.is_nonempty() ||
         parsed_.password.is_nonempty())) {
      return true;
    }
    if (!new_is_file && old_is_file && parsed_.host.is_empty()) {
      return true;
    }
  }
  url::Replacements<char> replacements;
  replacements.SetSchemeStr(new_protocol);
  ReplaceComponents(replacements);
  return true;
}

void ResolvedURL::SetUser(std::u16string_view user) {
  if (user.empty() && !parsed_.username.is_valid()) {
    return;
  }
  const std::string value = base::UTF16ToUTF8(user);
  url::Replacements<char> replacements;
  replacements.SetUsernameStr(value);
  ReplaceComponents(replacements);
}

void ResolvedURL::SetPass(std::u16string_view pass) {
  if (pass.empty() && !parsed_.password.is_valid()) {
    return;
  }
  const std::string value = base::UTF16ToUTF8(pass);
  url::Replacements<char> replacements;
  replacements.SetPasswordStr(value);
  ReplaceComponents(replacements);
}

void ResolvedURL::SetHost(std::u16string_view input) {
  const std::u16string host = RemoveUrlWhitespace(input);
  const std::string value = base::UTF16ToUTF8(FindHostPart(host, IsStandard()));
  url::Replacements<char> replacements;
  replacements.SetHostStr(value);
  ReplaceComponents(replacements);
}

void ResolvedURL::SetHostAndPort(std::u16string_view input) {
  // Blink's sloppy parse, kept for compatibility: the first colon after an
  // IPv6 address starts the port, and an empty host leaves the URL alone.
  const std::u16string whole = RemoveUrlWhitespace(input);
  const std::u16string_view host_and_port = FindHostPart(whole, IsStandard());
  size_t ipv6_terminator = host_and_port.rfind(']');
  if (ipv6_terminator == std::u16string_view::npos) {
    ipv6_terminator = !host_and_port.empty() && host_and_port.front() == '['
                          ? host_and_port.size()
                          : 0;
  }
  const size_t colon = host_and_port.find(':', ipv6_terminator);
  if (colon == 0) {
    return;
  }
  std::u16string_view host = host_and_port;
  std::u16string_view port;
  if (colon != std::u16string_view::npos) {
    host = host_and_port.substr(0, colon);
    port = ParsePortFromString(host_and_port.substr(colon + 1));
  }
  {
    const std::string value = base::UTF16ToUTF8(host);
    url::Replacements<char> replacements;
    replacements.SetHostStr(value);
    ReplaceComponents(replacements);
  }
  if (is_valid_ && !port.empty()) {
    const std::string value = base::UTF16ToUTF8(port);
    url::Replacements<char> replacements;
    replacements.SetPortStr(value);
    ReplaceComponents(replacements, /*preserve_validity=*/true);
  }
}

void ResolvedURL::RemovePort() {
  if (!HasPort()) {
    return;
  }
  url::Replacements<char> replacements;
  replacements.ClearPort();
  ReplaceComponents(replacements);
}

void ResolvedURL::SetPort(std::u16string_view input) {
  const std::u16string port = RemoveUrlWhitespace(input);
  const std::u16string_view parsed = ParsePortFromString(port);
  if (parsed.empty()) {
    return;
  }
  // Blink's StringToUintLoose(), which the leading zeros are gone for.
  uint32_t value = 0;
  if (parsed.size() > 5 ||
      !base::StringToUint(base::UTF16ToUTF8(parsed), &value) || value > 65535) {
    return;
  }
  if (IsDefaultPortForProtocol(static_cast<uint16_t>(value), Protocol())) {
    RemovePort();
    return;
  }
  const std::string port_string = base::NumberToString(value);
  url::Replacements<char> replacements;
  replacements.SetPortStr(port_string);
  ReplaceComponents(replacements);
}

void ResolvedURL::SetPath(std::u16string_view input) {
  const std::string value = base::UTF16ToUTF8(RemoveUrlWhitespace(input));
  url::Replacements<char> replacements;
  replacements.SetPathStr(value);
  ReplaceComponents(replacements);
}

void ResolvedURL::SetQuery(const std::optional<std::u16string>& input) {
  url::Replacements<char> replacements;
  std::string value;
  if (!input) {
    replacements.ClearQuery();
  } else {
    value = base::UTF16ToUTF8(RemoveUrlWhitespace(*input));
    replacements.SetQueryStr(!value.empty() && value[0] == '?'
                                 ? std::string_view(value).substr(1)
                                 : std::string_view(value));
  }
  ReplaceComponents(replacements);
}

void ResolvedURL::SetFragmentIdentifier(
    const std::optional<std::u16string>& input) {
  if (!input && !parsed_.ref.is_valid()) {
    return;
  }
  url::Replacements<char> replacements;
  std::string value;
  if (!input) {
    replacements.ClearRef();
  } else {
    value = base::UTF16ToUTF8(RemoveUrlWhitespace(*input));
    replacements.SetRefStr(value);
  }
  ReplaceComponents(replacements);
}

void ResolvedURL::RemoveFragmentIdentifier() {
  if (!parsed_.ref.is_valid()) {
    return;
  }
  url::Replacements<char> replacements;
  replacements.ClearRef();
  ReplaceComponents(replacements);
}

}  // namespace mitto
