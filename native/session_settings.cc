#include "mitto/session_settings.h"

#include <utility>

#include "base/json/json_reader.h"
#include "base/strings/strcat.h"
#include "base/values.h"
#include "mitto/mitto.h"
#include "net/base/hash_value.h"
#include "net/base/host_port_pair.h"
#include "net/http/http_util.h"

namespace mitto {
namespace {

constexpr std::string_view kVersionKey = "version";
constexpr std::string_view kIgnoreCertificateErrorsSPKIListKey =
    "ignore_certificate_errors_spki_list";
constexpr std::string_view kAcceptLanguageKey = "accept_language";
constexpr std::string_view kOriginsToForceQuicOnKey =
    "origins_to_force_quic_on";
constexpr std::string_view kEveryOrigin = "*";

std::optional<SPKIHashSet> ReadSPKIHashes(const base::Value& value) {
  if (!value.is_list()) {
    return std::nullopt;
  }
  SPKIHashSet hashes;
  for (const base::Value& entry : value.GetList()) {
    if (!entry.is_string()) {
      return std::nullopt;
    }
    const std::optional<net::HashValue> hash = net::HashValue::FromString(
        base::StrCat({"sha256/", entry.GetString()}));
    if (!hash) {
      return std::nullopt;
    }
    hashes.insert(hash->sha256hashvalue());
  }
  return hashes;
}

// Reads the origins the way Chrome reads --origin-to-force-quic-on in
// network_session_configurator::ParseCommandLineAndFieldTrials(), except that
// an entry that names no valid origin is refused. Chrome would keep it, and
// //net would then accept unknown roots for every host.
bool ReadOriginsToForceQuicOn(const base::Value& value,
                              SessionSettings& settings) {
  if (!value.is_list()) {
    return false;
  }
  for (const base::Value& entry : value.GetList()) {
    if (!entry.is_string()) {
      return false;
    }
    if (entry.GetString() == kEveryOrigin) {
      settings.force_quic_everywhere = true;
      continue;
    }
    const net::HostPortPair host_port =
        net::HostPortPair::FromString(entry.GetString());
    url::SchemeHostPort origin(url::kHttpsScheme, host_port.host(),
                               host_port.port());
    if (!origin.IsValid()) {
      return false;
    }
    settings.origins_to_force_quic_on.insert(std::move(origin));
  }
  return true;
}

}  // namespace

SessionSettings::SessionSettings() = default;
SessionSettings::SessionSettings(SessionSettings&&) = default;
SessionSettings& SessionSettings::operator=(SessionSettings&&) = default;
SessionSettings::~SessionSettings() = default;

std::optional<SessionSettings> ParseSessionSettings(std::string_view json) {
  const std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!dict || dict->FindInt(kVersionKey) != MITTO_SETTINGS_VERSION) {
    return std::nullopt;
  }

  SessionSettings settings;
  for (const auto [key, value] : *dict) {
    if (key == kVersionKey) {
      continue;
    }
    if (key == kAcceptLanguageKey) {
      // //net CHECKs the value of each header it adds.
      if (!value.is_string() ||
          !net::HttpUtil::IsValidHeaderValue(value.GetString())) {
        return std::nullopt;
      }
      settings.accept_language = value.GetString();
      continue;
    }
    if (key == kOriginsToForceQuicOnKey) {
      if (!ReadOriginsToForceQuicOn(value, settings)) {
        return std::nullopt;
      }
      continue;
    }
    if (key != kIgnoreCertificateErrorsSPKIListKey) {
      return std::nullopt;
    }
    std::optional<SPKIHashSet> hashes = ReadSPKIHashes(value);
    if (!hashes) {
      return std::nullopt;
    }
    settings.ignore_certificate_errors_spki_list = std::move(*hashes);
  }
  return settings;
}

}  // namespace mitto
