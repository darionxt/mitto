#include "mitto/cookie_json.h"

#include <stdint.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/containers/span.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/notreached.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "mitto/mitto.h"
#include "net/base/url_util.h"
#include "net/cookies/cookie_access_params.h"
#include "net/cookies/cookie_access_result.h"
#include "net/cookies/cookie_constants.h"
#include "net/cookies/cookie_options.h"
#include "net/cookies/cookie_partition_key.h"
#include "url/gurl.h"
#include "url/third_party/mozilla/url_parse.h"
#include "url/url_canon.h"
#include "url/url_constants.h"

namespace mitto {
namespace {

constexpr std::string_view kVersionKey = "version";
constexpr std::string_view kCookiesKey = "cookies";
constexpr std::string_view kNameKey = "name";
constexpr std::string_view kValueKey = "value";
constexpr std::string_view kDomainKey = "domain";
constexpr std::string_view kPathKey = "path";
constexpr std::string_view kSecureKey = "secure";
constexpr std::string_view kHttpOnlyKey = "http_only";
constexpr std::string_view kSameSiteKey = "same_site";
constexpr std::string_view kPriorityKey = "priority";
constexpr std::string_view kExpiresKey = "expires_us";
constexpr std::string_view kCreatedKey = "created_us";
constexpr std::string_view kLastAccessedKey = "last_accessed_us";
constexpr std::string_view kLastUpdatedKey = "last_updated_us";
constexpr std::string_view kPartitionKeyKey = "partition_key";
constexpr std::string_view kTopLevelSiteKey = "top_level_site";
constexpr std::string_view kHasCrossSiteAncestorKey = "has_cross_site_ancestor";
constexpr std::string_view kSourceSchemeKey = "source_scheme";
constexpr std::string_view kSourcePortKey = "source_port";
constexpr std::string_view kSourceTypeKey = "source_type";

// A JSON number is a double, which holds every whole number up to this exactly.
constexpr double kMaxTimeMicroseconds = 9007199254740992.0;

constexpr int kMaxSourcePort = 65535;

// The port of the URL that a new cookie counts as coming from.
constexpr int kHttpsPort = 443;

template <typename T>
struct NamedValue {
  std::string_view name;
  T value;
};

constexpr NamedValue<net::CookieSameSite> kSameSiteNames[] = {
    {"unspecified", net::CookieSameSite::UNSPECIFIED},
    {"none", net::CookieSameSite::NO_RESTRICTION},
    {"lax", net::CookieSameSite::LAX_MODE},
    {"strict", net::CookieSameSite::STRICT_MODE},
};

constexpr NamedValue<net::CookiePriority> kPriorityNames[] = {
    {"low", net::COOKIE_PRIORITY_LOW},
    {"medium", net::COOKIE_PRIORITY_MEDIUM},
    {"high", net::COOKIE_PRIORITY_HIGH},
};

constexpr NamedValue<net::CookieSourceScheme> kSourceSchemeNames[] = {
    {"unset", net::CookieSourceScheme::kUnset},
    {"non_secure", net::CookieSourceScheme::kNonSecure},
    {"secure", net::CookieSourceScheme::kSecure},
};

constexpr NamedValue<net::CookieSourceType> kSourceTypeNames[] = {
    {"http", net::CookieSourceType::kHTTP},
    {"script", net::CookieSourceType::kScript},
    {"other", net::CookieSourceType::kOther},
};

template <typename T>
std::string_view NameOf(base::span<const NamedValue<T>> names, T value) {
  for (const NamedValue<T>& named_value : names) {
    if (named_value.value == value) {
      return named_value.name;
    }
  }
  NOTREACHED();
}

// A cookie's bytes have no fixed encoding, and JSON holds text, so each byte
// goes as the character of that number. Nothing is lost that way, whatever a
// server sent.
std::string BytesToText(std::string_view bytes) {
  std::u16string text;
  text.reserve(bytes.size());
  for (const char byte : bytes) {
    text.push_back(static_cast<uint8_t>(byte));
  }
  return base::UTF16ToUTF8(text);
}

// Returns nullopt if `text` holds a character that is not one byte.
std::optional<std::string> TextToBytes(std::string_view text) {
  std::string bytes;
  bytes.reserve(text.size());
  for (const char16_t character : base::UTF8ToUTF16(text)) {
    if (character > 0xff) {
      return std::nullopt;
    }
    bytes.push_back(static_cast<char>(character));
  }
  return bytes;
}

// Leaves out a null time, which is what a session cookie's expiry is.
void SetTime(base::DictValue& dict, std::string_view key, base::Time time) {
  if (!time.is_null()) {
    dict.Set(key, static_cast<double>(
                      (time - base::Time::UnixEpoch()).InMicroseconds()));
  }
}

bool ReadBytes(const base::Value& value, std::string& out) {
  if (!value.is_string()) {
    return false;
  }
  std::optional<std::string> bytes = TextToBytes(value.GetString());
  if (!bytes) {
    return false;
  }
  out = std::move(*bytes);
  return true;
}

bool ReadString(const base::Value& value, std::string& out) {
  if (!value.is_string()) {
    return false;
  }
  out = value.GetString();
  return true;
}

bool ReadBool(const base::Value& value, bool& out) {
  const std::optional<bool> boolean = value.GetIfBool();
  out = boolean.value_or(false);
  return boolean.has_value();
}

bool ReadTime(const base::Value& value, std::optional<base::Time>& out) {
  // An integer that does not fit an int is a double to base::Value. A whole
  // number of microseconds has to stay what it was.
  const std::optional<double> microseconds = value.GetIfDouble();
  if (!microseconds || *microseconds < 0 ||
      *microseconds > kMaxTimeMicroseconds) {
    return false;
  }
  const int64_t whole_microseconds = static_cast<int64_t>(*microseconds);
  if (static_cast<double>(whole_microseconds) != *microseconds) {
    return false;
  }
  out = base::Time::UnixEpoch() + base::Microseconds(whole_microseconds);
  return true;
}

template <typename T>
bool ReadName(const base::Value& value,
              base::span<const NamedValue<T>> names,
              T& out) {
  if (!value.is_string()) {
    return false;
  }
  for (const NamedValue<T>& named_value : names) {
    if (value.GetString() == named_value.name) {
      out = named_value.value;
      return true;
    }
  }
  return false;
}

bool ReadPartitionKey(const base::Value& value,
                      std::optional<net::CookiePartitionKey>& out) {
  if (!value.is_dict()) {
    return false;
  }
  const base::DictValue& dict = value.GetDict();
  const std::string* const top_level_site = dict.FindString(kTopLevelSiteKey);
  const std::optional<bool> has_cross_site_ancestor =
      dict.FindBool(kHasCrossSiteAncestorKey);
  if (!top_level_site || !has_cross_site_ancestor || dict.size() != 2) {
    return false;
  }
  base::expected<net::CookiePartitionKey, std::string> key =
      net::CookiePartitionKey::FromUntrustedInput(*top_level_site,
                                                  *has_cross_site_ancestor);
  if (!key.has_value()) {
    return false;
  }
  out = std::move(key).value();
  return true;
}

// What the JSON says about one cookie.
struct CookieFields {
  std::optional<std::string> name;
  std::optional<std::string> value;
  std::optional<std::string> domain;
  std::string path = "/";
  bool secure = false;
  bool http_only = false;
  net::CookieSameSite same_site = net::CookieSameSite::UNSPECIFIED;
  net::CookiePriority priority = net::COOKIE_PRIORITY_DEFAULT;
  std::optional<base::Time> expires;
  std::optional<base::Time> created;
  std::optional<base::Time> last_accessed;
  std::optional<base::Time> last_updated;
  std::optional<net::CookiePartitionKey> partition_key;
  std::optional<net::CookieSourceScheme> source_scheme;
  std::optional<int> source_port;
  std::optional<net::CookieSourceType> source_type;
};

// Returns false if `key` is not a field, or `value` does not suit it.
bool ReadField(std::string_view key,
               const base::Value& value,
               CookieFields& fields) {
  if (key == kNameKey) {
    return ReadBytes(value, fields.name.emplace());
  }
  if (key == kValueKey) {
    return ReadBytes(value, fields.value.emplace());
  }
  if (key == kPathKey) {
    return ReadBytes(value, fields.path);
  }
  if (key == kDomainKey) {
    return ReadString(value, fields.domain.emplace());
  }
  if (key == kSecureKey) {
    return ReadBool(value, fields.secure);
  }
  if (key == kHttpOnlyKey) {
    return ReadBool(value, fields.http_only);
  }
  if (key == kSameSiteKey) {
    return ReadName<net::CookieSameSite>(value, kSameSiteNames,
                                         fields.same_site);
  }
  if (key == kPriorityKey) {
    return ReadName<net::CookiePriority>(value, kPriorityNames,
                                         fields.priority);
  }
  if (key == kExpiresKey) {
    return ReadTime(value, fields.expires);
  }
  if (key == kCreatedKey) {
    return ReadTime(value, fields.created);
  }
  if (key == kLastAccessedKey) {
    return ReadTime(value, fields.last_accessed);
  }
  if (key == kLastUpdatedKey) {
    return ReadTime(value, fields.last_updated);
  }
  if (key == kPartitionKeyKey) {
    return ReadPartitionKey(value, fields.partition_key);
  }
  if (key == kSourceSchemeKey) {
    return ReadName<net::CookieSourceScheme>(value, kSourceSchemeNames,
                                             fields.source_scheme.emplace());
  }
  if (key == kSourceTypeKey) {
    return ReadName<net::CookieSourceType>(value, kSourceTypeNames,
                                           fields.source_type.emplace());
  }
  if (key == kSourcePortKey) {
    // CanonicalCookie uses url::PORT_UNSPECIFIED and url::PORT_INVALID as well.
    fields.source_port = value.GetIfInt();
    return fields.source_port && *fields.source_port >= url::PORT_INVALID &&
           *fields.source_port <= kMaxSourcePort;
  }
  return false;
}

// A cookie's domain, without the leading dot that makes it a domain cookie.
std::string_view HostOf(std::string_view domain) {
  return domain.starts_with('.') ? domain.substr(1) : domain;
}

// A cookie the store has not seen yet. //net checks it as DevTools has it do:
// as if a server at its domain had sent it.
std::unique_ptr<net::CanonicalCookie> NewCookie(CookieFields fields) {
  if (fields.last_accessed || fields.last_updated || fields.source_scheme ||
      fields.source_port || fields.source_type) {
    return nullptr;
  }
  const std::string_view domain = *fields.domain;
  // On its own, as a URL it would be read past a port, a path or a user name.
  url::CanonHostInfo host_info;
  const std::string host = net::CanonicalizeHost(HostOf(domain), &host_info);
  if (host.empty() ||
      (!host_info.IsIPAddress() && !net::IsCanonicalizedHostCompliant(host))) {
    return nullptr;
  }
  const base::Time now = base::Time::Now();
  return net::CanonicalCookie::CreateSanitizedCookie(
      GURL(base::StrCat({"https://", host})), *fields.name, *fields.value,
      domain.starts_with('.') ? *fields.domain : std::string(),
      fields.path, now, fields.expires.value_or(base::Time()), now,
      fields.secure, fields.http_only, fields.same_site, fields.priority,
      fields.partition_key, /*status=*/nullptr);
}

// A cookie that was in a store. Any cookie //net has stored passes these
// checks, so that nothing a server sends can stop a saved list from loading.
// The checks of a new cookie would refuse some of those, and change the path of
// others.
std::unique_ptr<net::CanonicalCookie> StoredCookie(CookieFields fields) {
  const base::Time created = *fields.created;
  const net::CookieSourceScheme source_scheme =
      fields.source_scheme.value_or(net::CookieSourceScheme::kSecure);
  return net::CanonicalCookie::FromStorage(
      *std::move(fields.name), *std::move(fields.value),
      *std::move(fields.domain), std::move(fields.path), created,
      // The store accepts no cookie that lasts longer than this, and only one
      // that a person wrote into a file can.
      net::CanonicalCookie::ValidateAndAdjustExpiryDate(
          fields.expires.value_or(base::Time()), created, source_scheme),
      fields.last_accessed.value_or(created),
      fields.last_updated.value_or(created), fields.secure, fields.http_only,
      fields.same_site, fields.priority, std::move(fields.partition_key),
      source_scheme, fields.source_port.value_or(kHttpsPort),
      fields.source_type.value_or(net::CookieSourceType::kOther),
      net::CanonicalCookieFromStorageCallSite::kCookieManager);
}

std::optional<CookieToSet> ParseCookie(const base::Value& value) {
  if (!value.is_dict()) {
    return std::nullopt;
  }
  CookieFields fields;
  for (const auto [key, field] : value.GetDict()) {
    if (!ReadField(key, field, fields)) {
      return std::nullopt;
    }
  }
  if (!fields.name || !fields.value || !fields.domain) {
    return std::nullopt;
  }

  std::unique_ptr<net::CanonicalCookie> cookie =
      fields.created ? StoredCookie(std::move(fields))
                     : NewCookie(std::move(fields));
  if (!cookie || cookie->Domain().empty()) {
    return std::nullopt;
  }
  GURL source_url(base::StrCat({"https://", HostOf(cookie->Domain())}));
  if (!source_url.is_valid() || !cookie->IsDomainMatch(source_url.host())) {
    return std::nullopt;
  }
  // What the store will ask, as it does with no delegate, such as whether a
  // SameSite=None cookie is Secure. Asking now means that a list it would
  // refuse part of changes nothing.
  const net::CookieAccessResult access_result = cookie->IsSetPermittedInContext(
      source_url, net::CookieOptions::MakeAllInclusive(),
      net::CookieAccessParams(net::CookieAccessSemantics::UNKNOWN,
                              net::CookieScopeSemantics::UNKNOWN,
                              /*delegate_treats_url_as_trustworthy=*/false),
      {url::kHttpsScheme});
  if (!access_result.status.IsInclude()) {
    return std::nullopt;
  }

  CookieToSet cookie_to_set;
  cookie_to_set.cookie = std::move(cookie);
  cookie_to_set.source_url = std::move(source_url);
  return cookie_to_set;
}

}  // namespace

CookieToSet::CookieToSet() = default;
CookieToSet::CookieToSet(CookieToSet&&) = default;
CookieToSet& CookieToSet::operator=(CookieToSet&&) = default;
CookieToSet::~CookieToSet() = default;

CookieFilter::CookieFilter() = default;
CookieFilter::CookieFilter(CookieFilter&&) = default;
CookieFilter& CookieFilter::operator=(CookieFilter&&) = default;
CookieFilter::~CookieFilter() = default;

bool CookieFilter::Matches(const net::CanonicalCookie& cookie) const {
  return (!name || *name == cookie.Name()) &&
         (!domain || *domain == cookie.Domain()) &&
         (!path || *path == cookie.Path());
}

std::optional<std::vector<CookieToSet>> ParseCookies(std::string_view json) {
  const std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!dict || dict->FindInt(kVersionKey) != MITTO_COOKIES_VERSION ||
      dict->size() != 2) {
    return std::nullopt;
  }
  const base::ListValue* const list = dict->FindList(kCookiesKey);
  if (!list) {
    return std::nullopt;
  }

  std::vector<CookieToSet> cookies;
  cookies.reserve(list->size());
  for (const base::Value& value : *list) {
    std::optional<CookieToSet> cookie = ParseCookie(value);
    if (!cookie) {
      return std::nullopt;
    }
    cookies.push_back(*std::move(cookie));
  }
  return cookies;
}

std::optional<CookieFilter> ParseCookieFilter(std::string_view json) {
  const std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!dict || dict->FindInt(kVersionKey) != MITTO_COOKIES_VERSION) {
    return std::nullopt;
  }

  CookieFilter filter;
  for (const auto [key, value] : *dict) {
    if (key == kVersionKey) {
      continue;
    }
    const bool valid =
        key == kNameKey     ? ReadBytes(value, filter.name.emplace())
        : key == kPathKey   ? ReadBytes(value, filter.path.emplace())
        : key == kDomainKey ? ReadString(value, filter.domain.emplace())
                            : false;
    if (!valid) {
      return std::nullopt;
    }
  }
  return filter;
}

std::string WriteCookies(const net::CookieList& cookies) {
  base::ListValue list;
  for (const net::CanonicalCookie& cookie : cookies) {
    base::DictValue dict;
    dict.Set(kNameKey, BytesToText(cookie.Name()));
    dict.Set(kValueKey, BytesToText(cookie.Value()));
    dict.Set(kDomainKey, cookie.Domain());
    dict.Set(kPathKey, BytesToText(cookie.Path()));
    dict.Set(kSecureKey, cookie.SecureAttribute());
    dict.Set(kHttpOnlyKey, cookie.IsHttpOnly());
    dict.Set(kSameSiteKey,
             NameOf<net::CookieSameSite>(kSameSiteNames, cookie.SameSite()));
    dict.Set(kPriorityKey,
             NameOf<net::CookiePriority>(kPriorityNames, cookie.Priority()));
    SetTime(dict, kExpiresKey, cookie.ExpiryDate());
    SetTime(dict, kCreatedKey, cookie.CreationDate());
    SetTime(dict, kLastAccessedKey, cookie.LastAccessDate());
    SetTime(dict, kLastUpdatedKey, cookie.LastUpdateDate());
    if (cookie.IsPartitioned()) {
      const auto key =
          net::CookiePartitionKey::Serialize(cookie.PartitionKey());
      if (!key.has_value()) {
        continue;
      }
      base::DictValue partition_key;
      partition_key.Set(kTopLevelSiteKey, key->TopLevelSite());
      partition_key.Set(kHasCrossSiteAncestorKey,
                        key->has_cross_site_ancestor());
      dict.Set(kPartitionKeyKey, std::move(partition_key));
    }
    dict.Set(kSourceSchemeKey, NameOf<net::CookieSourceScheme>(
                                   kSourceSchemeNames, cookie.SourceScheme()));
    dict.Set(kSourcePortKey, cookie.SourcePort());
    dict.Set(kSourceTypeKey, NameOf<net::CookieSourceType>(
                                 kSourceTypeNames, cookie.SourceType()));
    list.Append(std::move(dict));
  }

  base::DictValue root;
  root.Set(kVersionKey, MITTO_COOKIES_VERSION);
  root.Set(kCookiesKey, std::move(list));
  // Without this option a whole double is written with a fraction or exponent.
  std::string json;
  CHECK(base::JSONWriter::WriteWithOptions(
      root, base::JSONWriter::OPTIONS_OMIT_DOUBLE_TYPE_PRESERVATION, &json));
  return json;
}

}  // namespace mitto
