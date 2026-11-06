#include "mitto/request_options.h"

#include <utility>

#include "base/json/json_reader.h"
#include "base/strings/string_util.h"
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

// Chrome's network service refuses the methods that the Fetch standard forbids,
// in network::cors::IsForbiddenMethod(), so Chrome never sends them.
bool IsValidMethod(std::string_view method) {
  const std::string upper_method = base::ToUpperASCII(method);
  return net::HttpUtil::IsToken(method) &&
         upper_method != net::HttpRequestHeaders::kConnectMethod &&
         upper_method != net::HttpRequestHeaders::kTraceMethod &&
         upper_method != net::HttpRequestHeaders::kTrackMethod;
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
