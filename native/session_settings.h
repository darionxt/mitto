#ifndef MITTO_SESSION_SETTINGS_H_
#define MITTO_SESSION_SETTINGS_H_

#include <optional>
#include <string_view>

#include "mitto/ignore_errors_cert_verifier.h"

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
};

// Reads the JSON format described at mitto_session_create() in mitto.h. Returns
// nullopt if `json` is not valid in that format. `json` comes from outside the
// library, so anything may be in it.
std::optional<SessionSettings> ParseSessionSettings(std::string_view json);

}  // namespace mitto

#endif  // MITTO_SESSION_SETTINGS_H_
