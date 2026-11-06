#include "mitto/session_settings.h"

#include <memory>
#include <utility>

#include "base/json/json_reader.h"
#include "base/strings/strcat.h"
#include "base/strings/string_split.h"
#include "base/values.h"
#include "mitto/mitto.h"
#include "net/base/hash_value.h"
#include "net/base/host_mapping_rules.h"
#include "net/base/host_port_pair.h"
#include "net/base/ip_address.h"
#include "net/cert/x509_util.h"
#include "net/http/http_util.h"
#include "third_party/boringssl/src/pki/pem.h"

namespace mitto {
namespace {

constexpr std::string_view kVersionKey = "version";
constexpr std::string_view kIgnoreCertificateErrorsSPKIListKey =
    "ignore_certificate_errors_spki_list";
constexpr std::string_view kAdditionalTrustAnchorsKey =
    "additional_trust_anchors";
constexpr std::string_view kPemBlockTypes[] = {"CERTIFICATE"};
constexpr std::string_view kAcceptLanguageKey = "accept_language";
constexpr std::string_view kOriginsToForceQuicOnKey =
    "origins_to_force_quic_on";
constexpr std::string_view kEveryOrigin = "*";
constexpr std::string_view kDnsNameserverKey = "dns_nameserver";
constexpr std::string_view kHostResolverRulesKey = "host_resolver_rules";

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

// Reads each entry as the text of a PEM file. An entry with no certificate is
// refused, and so is a certificate that //net cannot parse, where Chrome's own
// reading of its additional certificates drops it. Either way the caller's
// server would just fail later, with nothing to say why.
std::optional<bssl::ParsedCertificateList> ReadTrustAnchors(
    const base::Value& value) {
  if (!value.is_list()) {
    return std::nullopt;
  }
  bssl::ParsedCertificateList anchors;
  for (const base::Value& entry : value.GetList()) {
    if (!entry.is_string()) {
      return std::nullopt;
    }
    bssl::PEMTokenizer pem(entry.GetString(), kPemBlockTypes);
    if (!pem.GetNext()) {
      return std::nullopt;
    }
    do {
      std::shared_ptr<const bssl::ParsedCertificate> anchor =
          net::x509_util::CreateParsedCertificate(
              net::x509_util::CreateCryptoBuffer(pem.data()),
              /*errors=*/nullptr);
      if (!anchor) {
        return std::nullopt;
      }
      anchors.push_back(std::move(anchor));
    } while (pem.GetNext());
  }
  return anchors;
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

std::optional<net::IPEndPoint> ReadDnsNameserver(const base::Value& value) {
  if (!value.is_string()) {
    return std::nullopt;
  }
  const net::HostPortPair host_port =
      net::HostPortPair::FromString(value.GetString());
  const std::optional<net::IPAddress> address =
      net::IPAddress::FromIPLiteral(host_port.host());
  // Nothing listens on port 0, which is also what an entry without a valid port
  // comes to.
  if (!address || host_port.port() == 0) {
    return std::nullopt;
  }
  return net::IPEndPoint(*address, host_port.port());
}

// Reads the rules the way net::HostMappingRules::SetRulesFromString() does,
// except that a rule it cannot parse is refused. It would log that and go on
// without the rule, so the name would reach the real resolver.
bool ReadHostResolverRules(const base::Value& value,
                           SessionSettings& settings) {
  if (!value.is_string()) {
    return false;
  }
  net::HostMappingRules rules;
  if (!value.GetString().empty()) {
    for (std::string_view rule :
         base::SplitStringPiece(value.GetString(), ",", base::TRIM_WHITESPACE,
                                base::SPLIT_WANT_ALL)) {
      if (!rules.AddRuleFromString(rule)) {
        return false;
      }
    }
  }
  settings.host_resolver_rules = value.GetString();
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
    if (key == kAdditionalTrustAnchorsKey) {
      std::optional<bssl::ParsedCertificateList> anchors =
          ReadTrustAnchors(value);
      if (!anchors) {
        return std::nullopt;
      }
      settings.additional_trust_anchors = std::move(*anchors);
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
    if (key == kDnsNameserverKey) {
      settings.dns_nameserver = ReadDnsNameserver(value);
      if (!settings.dns_nameserver) {
        return std::nullopt;
      }
      continue;
    }
    if (key == kHostResolverRulesKey) {
      if (!ReadHostResolverRules(value, settings)) {
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
