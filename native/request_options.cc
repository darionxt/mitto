#include "mitto/request_options.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/json/json_reader.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "base/values.h"
#include "mitto/mitto.h"
#include "net/http/http_util.h"

namespace mitto {
namespace {

constexpr std::string_view kVersionKey = "version";
constexpr std::string_view kMethodKey = "method";
constexpr std::string_view kPriorityKey = "priority";
constexpr std::string_view kPriorityIncrementalKey = "priority_incremental";
constexpr std::string_view kReferrerKey = "referrer";
constexpr std::string_view kFollowRedirectsKey = "follow_redirects";
constexpr std::string_view kStreamBodyKey = "stream_body";
constexpr std::string_view kStreamBodySizeKey = "stream_body_size";
constexpr std::string_view kConnectTimeoutKey = "connect_timeout_ms";
constexpr std::string_view kReadTimeoutKey = "read_timeout_ms";
constexpr std::string_view kTotalTimeoutKey = "total_timeout_ms";
constexpr std::string_view kFetchKey = "fetch";
constexpr std::string_view kModeKey = "mode";
constexpr std::string_view kDestinationKey = "destination";
constexpr std::string_view kCredentialsModeKey = "credentials_mode";
constexpr std::string_view kHasUserActivationKey = "has_user_activation";
constexpr std::string_view kInitiatorKey = "initiator";
constexpr std::string_view kFixedHeadersKey = "fixed_headers";

// 2^53, the largest whole number up to which a double holds every one exactly.
constexpr double kMaxSize = 9007199254740992.0;

// Chrome's network service refuses the methods that the Fetch standard forbids,
// in network::cors::IsForbiddenMethod(), so Chrome never sends them.
bool IsValidMethod(std::string_view method) {
  const std::string upper_method = base::ToUpperASCII(method);
  return net::HttpUtil::IsToken(method) &&
         upper_method != net::HttpRequestHeaders::kConnectMethod &&
         upper_method != net::HttpRequestHeaders::kTraceMethod &&
         upper_method != net::HttpRequestHeaders::kTrackMethod;
}

// Returns where the timer that `key` names goes, or null if it names none.
std::optional<base::TimeDelta>* FindTimeout(RequestOptions& options,
                                            std::string_view key) {
  if (key == kConnectTimeoutKey) {
    return &options.connect_timeout;
  }
  if (key == kReadTimeoutKey) {
    return &options.read_timeout;
  }
  if (key == kTotalTimeoutKey) {
    return &options.total_timeout;
  }
  return nullptr;
}

// Returns nullopt if `value` is not a positive number of milliseconds.
std::optional<base::TimeDelta> ParseTimeout(const base::Value& value) {
  const std::optional<int> milliseconds = value.GetIfInt();
  if (!milliseconds || *milliseconds <= 0) {
    return std::nullopt;
  }
  return base::Milliseconds(*milliseconds);
}

// Returns nullopt if `value` is not a whole number of bytes that a double holds
// exactly, which is what base::Value makes of an integer that does not fit an int.
std::optional<uint64_t> ParseSize(const base::Value& value) {
  const std::optional<double> size = value.GetIfDouble();
  if (!size || *size < 0 || *size > kMaxSize) {
    return std::nullopt;
  }
  const uint64_t whole_size = static_cast<uint64_t>(*size);
  if (static_cast<double>(whole_size) != *size) {
    return std::nullopt;
  }
  return whole_size;
}

std::optional<RequestMode> ParseRequestMode(const base::Value& value) {
  if (!value.is_string()) {
    return std::nullopt;
  }
  if (value.GetString() == "navigate") {
    return RequestMode::kNavigate;
  }
  if (value.GetString() == "cors") {
    return RequestMode::kCors;
  }
  if (value.GetString() == "no-cors") {
    return RequestMode::kNoCors;
  }
  return std::nullopt;
}

std::optional<CredentialsMode> ParseCredentialsMode(const base::Value& value) {
  if (!value.is_string()) {
    return std::nullopt;
  }
  if (value.GetString() == "same-origin") {
    return CredentialsMode::kSameOrigin;
  }
  if (value.GetString() == "include") {
    return CredentialsMode::kInclude;
  }
  return std::nullopt;
}

std::optional<FetchParams> ParseFetchParams(const base::Value& value) {
  if (!value.is_dict()) {
    return std::nullopt;
  }
  FetchParams params;
  bool has_mode = false;
  bool has_credentials_mode = false;
  for (const auto [key, item] : value.GetDict()) {
    if (key == kModeKey) {
      const std::optional<RequestMode> mode = ParseRequestMode(item);
      if (!mode) {
        return std::nullopt;
      }
      params.mode = *mode;
      has_mode = true;
    } else if (key == kDestinationKey) {
      if (!item.is_string() || !net::HttpUtil::IsToken(item.GetString())) {
        return std::nullopt;
      }
      params.destination = item.GetString();
    } else if (key == kCredentialsModeKey) {
      const std::optional<CredentialsMode> credentials_mode =
          ParseCredentialsMode(item);
      if (!credentials_mode) {
        return std::nullopt;
      }
      params.credentials_mode = *credentials_mode;
      has_credentials_mode = true;
    } else if (key == kHasUserActivationKey) {
      const std::optional<bool> has_user_activation = item.GetIfBool();
      if (!has_user_activation) {
        return std::nullopt;
      }
      params.has_user_activation = *has_user_activation;
    } else if (key == kInitiatorKey) {
      if (!item.is_string()) {
        return std::nullopt;
      }
      params.initiator = GURL(item.GetString());
      if (!params.initiator->is_valid()) {
        return std::nullopt;
      }
    } else if (key == kFixedHeadersKey) {
      if (!item.is_list()) {
        return std::nullopt;
      }
      for (const base::Value& name : item.GetList()) {
        if (!name.is_string()) {
          return std::nullopt;
        }
        params.fixed_headers.insert(base::ToLowerASCII(name.GetString()));
      }
    } else {
      return std::nullopt;
    }
  }
  if (!has_mode || !has_credentials_mode || params.destination.empty()) {
    return std::nullopt;
  }
  return params;
}

}  // namespace

FetchParams::FetchParams() = default;
FetchParams::FetchParams(FetchParams&&) = default;
FetchParams& FetchParams::operator=(FetchParams&&) = default;
FetchParams::~FetchParams() = default;

RequestOptions::RequestOptions() = default;
RequestOptions::RequestOptions(RequestOptions&&) = default;
RequestOptions& RequestOptions::operator=(RequestOptions&&) = default;
RequestOptions::~RequestOptions() = default;

std::optional<RequestOptions> ParseRequestOptions(std::string_view json) {
  const std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!dict || dict->FindInt(kVersionKey) != MITTO_REQUEST_OPTIONS_VERSION) {
    return std::nullopt;
  }

  RequestOptions options;
  for (const auto [key, value] : *dict) {
    if (key == kVersionKey) {
      continue;
    }
    if (key == kMethodKey) {
      if (!value.is_string() || !IsValidMethod(value.GetString())) {
        return std::nullopt;
      }
      options.method = value.GetString();
      continue;
    }
    if (key == kPriorityKey) {
      const std::optional<int> priority = value.GetIfInt();
      if (!priority || *priority < net::MINIMUM_PRIORITY ||
          *priority > net::MAXIMUM_PRIORITY) {
        return std::nullopt;
      }
      options.priority = static_cast<net::RequestPriority>(*priority);
      continue;
    }
    if (key == kPriorityIncrementalKey) {
      const std::optional<bool> priority_incremental = value.GetIfBool();
      if (!priority_incremental) {
        return std::nullopt;
      }
      options.priority_incremental = *priority_incremental;
      continue;
    }
    if (key == kFollowRedirectsKey) {
      const std::optional<bool> follow_redirects = value.GetIfBool();
      if (!follow_redirects) {
        return std::nullopt;
      }
      options.follow_redirects = *follow_redirects;
      continue;
    }
    if (key == kStreamBodyKey) {
      const std::optional<bool> stream_body = value.GetIfBool();
      if (!stream_body) {
        return std::nullopt;
      }
      options.stream_body = *stream_body;
      continue;
    }
    if (key == kStreamBodySizeKey) {
      options.stream_body_size = ParseSize(value);
      if (!options.stream_body_size) {
        return std::nullopt;
      }
      continue;
    }
    if (std::optional<base::TimeDelta>* const timeout =
            FindTimeout(options, key)) {
      *timeout = ParseTimeout(value);
      if (!*timeout) {
        return std::nullopt;
      }
      continue;
    }
    if (key == kFetchKey) {
      options.fetch = ParseFetchParams(value);
      if (!options.fetch) {
        return std::nullopt;
      }
      continue;
    }
    if (key != kReferrerKey || !value.is_string()) {
      return std::nullopt;
    }
    // //net would silently send no Referer for a URL that it cannot send as one.
    options.referrer = GURL(value.GetString()).GetAsReferrer();
    if (!options.referrer.is_valid()) {
      return std::nullopt;
    }
  }
  if (options.stream_body_size && !options.stream_body) {
    return std::nullopt;
  }
  return options;
}

}  // namespace mitto
