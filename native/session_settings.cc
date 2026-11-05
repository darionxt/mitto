#include "mitto/session_settings.h"

#include <utility>

#include "base/json/json_reader.h"
#include "base/strings/strcat.h"
#include "base/values.h"
#include "mitto/mitto.h"
#include "net/base/hash_value.h"

namespace mitto {
namespace {

constexpr std::string_view kVersionKey = "version";
constexpr std::string_view kIgnoreCertificateErrorsSPKIListKey =
    "ignore_certificate_errors_spki_list";

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
