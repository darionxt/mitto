#ifndef MITTO_SESSION_SETTINGS_H_
#define MITTO_SESSION_SETTINGS_H_

#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "base/files/file_path.h"
#include "mitto/ignore_errors_cert_verifier.h"
#include "mitto/socks5_auth_exchange.h"
#include "net/base/auth.h"
#include "net/base/ip_endpoint.h"
#include "net/proxy_resolution/proxy_config.h"
#include "third_party/boringssl/src/pki/parsed_certificate.h"
#include "url/scheme_host_port.h"

namespace mitto {

// What a Session is configured with when it is created.
struct SessionSettings {
  SessionSettings();
  SessionSettings(SessionSettings&&);
  SessionSettings& operator=(SessionSettings&&);
  ~SessionSettings();

  // Certificate chains that hold one of these keys are accepted without being
  // verified.
  SPKIHashSet ignore_certificate_errors_spki_list;

  // Certificates to trust as roots, beside the Chrome Root Store.
  bssl::ParsedCertificateList additional_trust_anchors;

  // The value of the Accept-Language header that //net adds to a request that
  // has none. Empty adds no header.
  std::string accept_language;

  // The value of the User-Agent header that //net adds to a request that has
  // none, and sends in what it makes by itself: a CONNECT and a fetch for a
  // certificate. Empty adds an empty header to a request, and none to a
  // CONNECT.
  std::string user_agent;

  // The one proxy that requests go through, or none, whatever the system's
  // settings say.
  net::ProxyConfig proxy_config;

  // What to answer that proxy with when it asks, over HTTP.
  std::optional<net::AuthCredentials> proxy_credentials;

  // The same for a SOCKS5 proxy, which //net has no way to answer.
  std::optional<Socks5Credentials> socks5_credentials;

  // Origins that are reached over QUIC, and over nothing else, and whether that
  // goes for every origin. //net accepts a certificate from an unknown root
  // over QUIC only for a host named here.
  std::set<url::SchemeHostPort> origins_to_force_quic_on;
  bool force_quic_everywhere = false;

  // The one DNS server to ask, in place of the system's. For tests only.
  std::optional<net::IPEndPoint> dns_nameserver;

  // What Chrome's --host-resolver-rules takes, each rule known to be valid.
  std::string host_resolver_rules;

  // Whether the Session keeps responses in memory.
  bool http_cache_in_memory = false;

  // The directory that the Session keeps responses in, which is absolute. It is
  // what Chrome calls http_cache_directory before
  // PopulateHttpCacheDirectories() adds Cache_Data to it.
  std::optional<base::FilePath> http_cache_directory;
};

// Reads the JSON format described at mitto_session_create() in mitto.h. Returns
// nullopt if `json` is not valid in that format. `json` comes from outside the
// library, so anything may be in it.
std::optional<SessionSettings> ParseSessionSettings(std::string_view json);

}  // namespace mitto

#endif  // MITTO_SESSION_SETTINGS_H_
