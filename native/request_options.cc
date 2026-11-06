#include "mitto/request_options.h"

#include <optional>
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
constexpr std::string_view kConnectTimeoutKey = "connect_timeout_ms";
constexpr std::string_view kReadTimeoutKey = "read_timeout_ms";
constexpr std::string_view kTotalTimeoutKey = "total_timeout_ms";

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

}  // namespace

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
    if (std::optional<base::TimeDelta>* const timeout =
            FindTimeout(options, key)) {
      *timeout = ParseTimeout(value);
      if (!*timeout) {
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
  return options;
}

}  // namespace mitto
