// Ported from the parts of Chromium that decide which client hints a request
// has, with the upstream names kept:
// services/network/public/cpp/client_hints.cc,
// third_party/blink/common/client_hints, components/client_hints/browser and
// content/browser/client_hints. The source export leaves those out.
//
// Upstream: services/network/public/cpp/client_hints.cc
// Upstream: services/network/public/cpp/parsed_headers.cc
// Upstream: third_party/blink/common/client_hints/client_hints.cc
// Upstream: components/client_hints/browser/client_hints.cc
// Upstream: content/browser/client_hints/client_hints.cc
// Upstream: content/browser/browsing_data/browsing_data_remover_impl.cc

#ifndef MITTO_CLIENT_HINTS_H_
#define MITTO_CLIENT_HINTS_H_

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "url/origin.h"

namespace net {
class HttpResponseHeaders;
}  // namespace net

namespace mitto {

// The names of client hints, in lower case. An upstream `WebClientHintsType`
// stands for one of them, and a line in a Persona says what it sends.
using EnabledClientHints = std::set<std::string, std::less<>>;

// Whether `name` is in network::GetClientHintToNameMap(), in any case.
bool IsClientHintHeader(std::string_view name);

// blink::IsClientHintSentByDefault(). The same hints are the only ones whose
// permissions policy allows every origin by default, so these are also what a
// subresource may send to another origin.
bool IsClientHintSentByDefault(std::string_view name);

// network::ParseClientHintsHeader(). Returns nullopt if `header` is not a list
// of tokens. A token that names no hint counts for nothing.
std::optional<EnabledClientHints> ParseClientHintsHeader(
    const std::string& header);

// What network::PopulateParsedHeaders() makes of a response, as far as client
// hints go.
struct ParsedClientHintsHeaders {
  ParsedClientHintsHeaders();
  ParsedClientHintsHeaders(ParsedClientHintsHeaders&&);
  ParsedClientHintsHeaders& operator=(ParsedClientHintsHeaders&&);
  ~ParsedClientHintsHeaders();

  std::optional<EnabledClientHints> accept_ch;
  std::optional<EnabledClientHints> critical_ch;

  // Whether Clear-Site-Data takes away what a server asked for before. Upstream
  // also calls this a reason to ignore the two above, which it leaves empty.
  bool clears_client_hints = false;
};

ParsedClientHintsHeaders ParseClientHintsHeaders(
    const net::HttpResponseHeaders& headers);

// What the origins of a Session have asked for: client_hints::ClientHints,
// which keeps it in a content setting that Chrome saves. A Session keeps it for
// as long as it lasts. Used only on the network thread.
class ClientHintsStore {
 public:
  ClientHintsStore();

  ClientHintsStore(const ClientHintsStore&) = delete;
  ClientHintsStore& operator=(const ClientHintsStore&) = delete;

  ~ClientHintsStore();

  // ClientHints::PersistClientHints(). What `origin` asked for before is gone,
  // and nothing happens for an origin that is not potentially trustworthy.
  void PersistClientHints(const url::Origin& origin, EnabledClientHints hints);

  // BrowsingDataRemover::ClearClientHintCacheAndReply().
  void ClearClientHints(const url::Origin& origin);

  // Whether `origin` has asked for the hint of this name, in any case. The
  // upstream list comes from ClientHints::GetAllowedClientHintsFromSource().
  bool IsEnabled(const url::Origin& origin, std::string_view name) const;

 private:
  std::map<url::Origin, EnabledClientHints> hints_;
};

}  // namespace mitto

#endif  // MITTO_CLIENT_HINTS_H_
