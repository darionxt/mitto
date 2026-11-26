#include "mitto/range_after_cache.h"

#include <array>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/http/http_byte_range.h"
#include "net/http/http_util.h"
#include "net/url_request/url_request.h"

namespace mitto {

namespace {

// A request with any of these goes past the cache, with its Range line where
// it is: kPassThroughHeaders, and kValidationHeaders, which the cache does not
// take with a range, or at all when empty.
constexpr auto kHeadersThatDisableTheCache = std::to_array<std::string_view>(
    {"If-Unmodified-Since", "If-Match", "If-Range", "If-Modified-Since",
     "If-None-Match"});

}  // namespace

std::optional<net::HttpRequestHeaders> HeadersAfterHttpCache(
    std::string_view method,
    int load_flags,
    const net::HttpRequestHeaders& headers) {
  const std::optional<std::string> range =
      headers.GetHeader(net::HttpRequestHeaders::kRange);
  if (!range || (load_flags & net::LOAD_DISABLE_CACHE) || method != "GET") {
    return std::nullopt;
  }
  for (const std::string_view name : kHeadersThatDisableTheCache) {
    if (headers.HasHeader(name)) {
      return std::nullopt;
    }
  }
  // PartialData::Init().
  std::vector<net::HttpByteRange> ranges;
  if (!net::HttpUtil::ParseRangeHeader(*range, &ranges) || ranges.size() != 1 ||
      !ranges[0].IsValid()) {
    return std::nullopt;
  }
  const net::HttpByteRange& byte_range = ranges[0];
  net::HttpRequestHeaders moved = headers;
  moved.RemoveHeader(net::HttpRequestHeaders::kRange);
  // PartialData::RestoreHeaders(), from the start of the range.
  const net::HttpByteRange restored =
      byte_range.IsSuffixByteRange()
          ? net::HttpByteRange::Suffix(byte_range.suffix_length())
          : net::HttpByteRange::Bounded(byte_range.first_byte_position(),
                                        byte_range.last_byte_position());
  moved.SetHeader(net::HttpRequestHeaders::kRange, restored.GetHeaderValue());
  return moved;
}

NoHttpCacheNetworkDelegate::NoHttpCacheNetworkDelegate() = default;
NoHttpCacheNetworkDelegate::~NoHttpCacheNetworkDelegate() = default;

int NoHttpCacheNetworkDelegate::OnBeforeStartTransaction(
    net::URLRequest* request,
    const net::HttpRequestHeaders& headers,
    OnBeforeStartTransactionCallback callback) {
  std::optional<net::HttpRequestHeaders> moved =
      HeadersAfterHttpCache(request->method(), request->load_flags(), headers);
  if (!moved) {
    return net::OK;
  }
  // URLRequestHttpJob takes new lines only from the callback, which may not
  // run before this returns.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(std::move(callback), net::OK, std::move(moved)));
  return net::ERR_IO_PENDING;
}

}  // namespace mitto
