#include "mitto/policy_allowlist.h"

#include <stddef.h>

#include <algorithm>
#include <utility>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "url/gurl.h"
#include "url/url_util.h"

namespace mitto {

namespace {

constexpr size_t kMaxLength = 1 << 16;

// ParseScheme().
bool IsScheme(std::string_view text) {
  return !text.empty() && base::IsAsciiAlpha(text[0]) &&
         std::ranges::all_of(text, [](char c) {
           return base::IsAsciiAlphaNumeric(c) || c == '+' || c == '-' ||
                  c == '.';
         });
}

// What ParseHost() takes after a wildcard, where only a trailing dot leaves an
// empty label.
bool IsHost(std::string_view text) {
  const std::vector<std::string_view> labels = base::SplitStringPiece(
      text, ".", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  for (size_t i = 0; i < labels.size(); ++i) {
    if ((labels[i].empty() && i + 1 < labels.size()) ||
        !std::ranges::all_of(labels[i], [](char c) {
          return base::IsAsciiAlphaNumeric(c) || c == '-';
        })) {
      return false;
    }
  }
  return !text.empty();
}

// FromOrigin().
void AddOrigin(const url::Origin& origin, PolicyAllowlist& allowlist) {
  if (std::optional<PolicyOrigin> parsed =
          PolicyOrigin::Parse(origin.Serialize(), /*with_wildcards=*/true)) {
    allowlist.origins.push_back(*std::move(parsed));
  }
}

}  // namespace

// static
std::optional<PolicyOrigin> PolicyOrigin::Parse(std::string_view text,
                                                bool with_wildcards) {
  PolicyOrigin origin;
  const size_t scheme_end = text.find_first_of(":/");
  if (scheme_end == std::string_view::npos ||
      !IsScheme(text.substr(0, scheme_end))) {
    return std::nullopt;
  }
  origin.scheme = base::ToLowerASCII(text.substr(0, scheme_end));
  text = text.substr(scheme_end);
  if (text == ":") {
    // A scheme alone is for every host.
    return with_wildcards ? std::optional(origin) : std::nullopt;
  }
  if (!text.starts_with("://")) {
    return std::nullopt;
  }
  text = text.substr(3);
  const size_t host_end = text.find_first_of(":/");
  std::string_view host = text.substr(0, host_end);
  if (host == "*") {
    origin.is_host_wildcard = true;
    host = std::string_view();
  } else {
    if (host.starts_with("*.")) {
      origin.is_host_wildcard = true;
      host = host.substr(2);
    }
    if (!IsHost(host)) {
      return std::nullopt;
    }
  }
  origin.host = base::ToLowerASCII(host);
  // The path that may follow the port says nothing of the origin.
  if (host_end != std::string_view::npos && text[host_end] == ':') {
    const std::string_view after = text.substr(host_end + 1);
    const std::string_view port = after.substr(0, after.find('/'));
    if (port == "*") {
      origin.is_port_wildcard = true;
    } else if (port.empty() ||
               !std::ranges::all_of(port, &base::IsAsciiDigit<char>) ||
               !base::StringToInt(port, &origin.port)) {
      return std::nullopt;
    }
  }
  if (!with_wildcards && (origin.is_host_wildcard || origin.is_port_wildcard)) {
    return std::nullopt;
  }
  return origin;
}

bool PolicyOrigin::Matches(const url::Origin& origin) const {
  const GURL url = origin.GetURL();
  const std::string url_scheme(url.GetScheme());
  if (host.empty() && !is_host_wildcard) {
    // SourceAllowScheme() of a scheme alone, for a scheme's secure one too.
    return scheme == url_scheme ||
           (scheme == url::kHttpScheme && url_scheme == url::kHttpsScheme) ||
           (scheme == url::kWsScheme && url_scheme == url::kWssScheme);
  }
  const std::string url_host(url.IsStandard() ? url.GetHost() : std::string());
  if (scheme != url_scheme ||
      !(is_host_wildcard
            ? host.empty() || url_host.ends_with(base::StrCat({".", host}))
            : host == url_host)) {
    return false;
  }
  // SourceAllowPort().
  const int default_port = url::DefaultPortForScheme(scheme);
  const int url_port = url.EffectiveIntPort();
  return is_port_wildcard || port == url_port ||
         (port == url::PORT_UNSPECIFIED && default_port == url_port) ||
         (url_port == url::PORT_UNSPECIFIED && port == default_port);
}

std::string PolicyOrigin::Serialize() const {
  if (host.empty() && !is_host_wildcard) {
    return base::StrCat({scheme, ":"});
  }
  std::string text = base::StrCat(
      {scheme, "://",
       is_host_wildcard ? (host.empty() ? "*" : base::StrCat({"*.", host}))
                        : host});
  if (is_port_wildcard) {
    text += ":*";
  }
  if (port != url::PORT_UNSPECIFIED) {
    base::StrAppend(&text, {":", base::NumberToString(port)});
  }
  return text;
}

bool PolicyAllowlist::Contains(const url::Origin& origin) const {
  if (all_origins || origin == self || (opaque_source && origin.opaque())) {
    return true;
  }
  return !origin.opaque() &&
         std::ranges::any_of(origins, [&](const PolicyOrigin& allowed) {
           return allowed.Matches(origin);
         });
}

PolicyAllowlist ParsePolicyAllowlist(base::span<const std::string> tokens,
                                     const url::Origin& self,
                                     const url::Origin* source,
                                     bool with_wildcards,
                                     std::vector<PolicyMessage>& messages) {
  PolicyAllowlist allowlist;
  // 'src' is the opaque origin that the frame's document is to have, if its URL
  // gives it one.
  const auto add_source = [&] {
    if (source->opaque()) {
      allowlist.opaque_source = true;
    } else {
      AddOrigin(*source, allowlist);
    }
  };
  if (tokens.empty()) {
    if (source) {
      add_source();
    } else {
      allowlist.self = self;
    }
  }
  for (const std::string& token : tokens) {
    if (!base::IsStringASCII(token)) {
      messages.push_back({.text = "Non-ASCII characters in origin."});
    } else if (base::EqualsCaseInsensitiveASCII(token, "'self'")) {
      allowlist.self = self;
    } else if (source && base::EqualsCaseInsensitiveASCII(token, "'src'")) {
      add_source();
    } else if (base::EqualsCaseInsensitiveASCII(token, "'none'")) {
      continue;
    } else if (token == "*") {
      allowlist.all_origins = true;
      allowlist.opaque_source = true;
    } else if (std::optional<PolicyOrigin> origin =
                   PolicyOrigin::Parse(token, with_wildcards)) {
      allowlist.origins.push_back(*std::move(origin));
    } else {
      messages.push_back(
          {.text = base::StrCat({"Unrecognized origin: '", token, "'."})});
    }
  }
  if (allowlist.all_origins) {
    allowlist.origins.clear();
  }
  std::ranges::sort(allowlist.origins);
  return allowlist;
}

std::optional<bool> PermissionsPolicyHeaders::Allows(
    std::string_view feature,
    const url::Origin& self,
    const url::Origin& origin) const {
  for (const PolicyNode* node : {&permissions_policy, &feature_policy}) {
    const auto entry =
        std::ranges::find(node->entries, feature, &PolicyEntry::feature);
    if (entry != node->entries.end()) {
      std::vector<PolicyMessage> messages;
      return ParsePolicyAllowlist(entry->tokens, self, /*source=*/nullptr,
                                  node == &permissions_policy, messages)
          .Contains(origin);
    }
  }
  return std::nullopt;
}

void PermissionsPolicyHeaders::DropMessages() {
  feature_policy.messages = std::vector<PolicyMessage>();
  permissions_policy.messages = std::vector<PolicyMessage>();
}

PolicyNode ParseFeaturePolicy(std::string_view policy,
                              size_t length,
                              bool is_header) {
  PolicyNode node;
  if (length > kMaxLength) {
    node.messages.push_back(
        {true, base::StrCat({"Feature policy declaration exceeds size limit(",
                             base::NumberToString(length), ">",
                             base::NumberToString(kMaxLength), ")"})});
    return node;
  }
  for (const std::string_view entry : base::SplitStringPiece(
           policy, is_header ? ",;" : ";", base::KEEP_WHITESPACE,
           base::SPLIT_WANT_NONEMPTY)) {
    std::vector<std::string> tokens = base::SplitString(
        entry, " \t\n\f\r", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
    if (!tokens.empty()) {
      std::string feature = std::move(tokens.front());
      tokens.erase(tokens.begin());
      node.entries.push_back({std::move(feature), std::move(tokens)});
    }
  }
  return node;
}

}  // namespace mitto
