#include "mitto/client_hints.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/strings/string_util.h"
#include "net/base/is_potentially_trustworthy.h"
#include "net/http/http_response_headers.h"
#include "net/http/structured_headers.h"
#include "net/url_request/clear_site_data.h"

namespace mitto {
namespace {

constexpr auto kClientHintNames = std::to_array<std::string_view>({
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

constexpr auto kClientHintsSentByDefault = std::to_array<std::string_view>({
    "save-data",
    "sec-ch-ua",
    "sec-ch-ua-mobile",
    "sec-ch-ua-platform",
});

bool IsListedIgnoringCase(base::span<const std::string_view> names,
                          std::string_view name) {
  return std::ranges::any_of(names, [name](std::string_view listed) {
    return base::EqualsCaseInsensitiveASCII(listed, name);
  });
}

}  // namespace

bool IsClientHintHeader(std::string_view name) {
  return IsListedIgnoringCase(kClientHintNames, name);
}

bool IsClientHintSentByDefault(std::string_view name) {
  return IsListedIgnoringCase(kClientHintsSentByDefault, name);
}

std::optional<EnabledClientHints> ParseClientHintsHeader(
    const std::string& header) {
  const std::optional<net::structured_headers::List> list =
      net::structured_headers::ParseList(header);
  if (!list) {
    return std::nullopt;
  }

  EnabledClientHints hints;
  for (const auto& list_item : *list) {
    const net::structured_headers::ParameterizedItem* const item =
        list_item.GetIfItem();
    if (!item || !item->item.is_token()) {
      return std::nullopt;
    }
    const std::string name = base::ToLowerASCII(item->item.GetToken());
    if (IsClientHintHeader(name)) {
      hints.insert(name);
    }
  }
  return hints;
}

ParsedClientHintsHeaders::ParsedClientHintsHeaders() = default;
ParsedClientHintsHeaders::ParsedClientHintsHeaders(ParsedClientHintsHeaders&&) =
    default;
ParsedClientHintsHeaders& ParsedClientHintsHeaders::operator=(
    ParsedClientHintsHeaders&&) = default;
ParsedClientHintsHeaders::~ParsedClientHintsHeaders() = default;

ParsedClientHintsHeaders ParseClientHintsHeaders(
    const net::HttpResponseHeaders& headers) {
  ParsedClientHintsHeaders parsed;
  const std::vector<std::string> cleared = net::ClearSiteDataHeaderContents(
      headers.GetNormalizedHeader(net::kClearSiteDataHeader)
          .value_or(std::string()));
  parsed.clears_client_hints =
      std::ranges::any_of(cleared, [](const std::string& type) {
        return type == net::kDatatypeCache ||
               type == net::kDatatypeClientHints ||
               type == net::kDatatypeCookies || type == net::kDatatypeWildcard;
      });
  if (parsed.clears_client_hints) {
    return parsed;
  }
  if (const std::optional<std::string> accept_ch =
          headers.GetNormalizedHeader("Accept-CH")) {
    parsed.accept_ch = ParseClientHintsHeader(*accept_ch);
  }
  if (const std::optional<std::string> critical_ch =
          headers.GetNormalizedHeader("Critical-CH")) {
    parsed.critical_ch = ParseClientHintsHeader(*critical_ch);
  }
  return parsed;
}

ClientHintsStore::ClientHintsStore() = default;
ClientHintsStore::~ClientHintsStore() = default;

void ClientHintsStore::PersistClientHints(const url::Origin& origin,
                                          EnabledClientHints hints) {
  if (net::IsOriginPotentiallyTrustworthy(origin)) {
    hints_[origin] = std::move(hints);
  }
}

void ClientHintsStore::ClearClientHints(const url::Origin& origin) {
  hints_.erase(origin);
}

bool ClientHintsStore::IsEnabled(const url::Origin& origin,
                                 std::string_view name) const {
  const auto found = hints_.find(origin);
  return found != hints_.end() &&
         found->second.contains(base::ToLowerASCII(name));
}

}  // namespace mitto
