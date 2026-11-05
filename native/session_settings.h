#ifndef MITTO_SESSION_SETTINGS_H_
#define MITTO_SESSION_SETTINGS_H_

#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "mitto/ignore_errors_cert_verifier.h"
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

  // The value of the Accept-Language header that //net adds to a request that
  // has none. Empty adds no header.
  std::string accept_language;

  // Origins that are reached over QUIC, and over nothing else, and whether that
  // goes for every origin. //net accepts a certificate from an unknown root
  // over QUIC only for a host named here.
  std::set<url::SchemeHostPort> origins_to_force_quic_on;
  bool force_quic_everywhere = false;
};

// Reads the JSON format described at mitto_session_create() in mitto.h. Returns
// nullopt if `json` is not valid in that format. `json` comes from outside the
// library, so anything may be in it.
std::optional<SessionSettings> ParseSessionSettings(std::string_view json);

}  // namespace mitto

#endif  // MITTO_SESSION_SETTINGS_H_
