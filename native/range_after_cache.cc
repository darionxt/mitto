#include "mitto/range_after_cache.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/http/http_cache_util.h"
#include "net/http/partial_data.h"
#include "net/url_request/url_request.h"

namespace mitto {

std::optional<net::HttpRequestHeaders> HeadersAfterHttpCache(
    std::string_view method,
    int load_flags,
    const net::HttpRequestHeaders& headers) {
  if (!headers.HasHeader(net::HttpRequestHeaders::kRange) || method != "GET") {
    return std::nullopt;
  }
  // HttpCache::Transaction::SetRequest(): the cache takes no range with a
  // validation header, nor any request with a header that is not valid, such
  // as an empty If-None-Match, or with one that passes it by, such as If-Range.
  load_flags |= net::http_cache_util::GetLoadFlagsForExtraHeaders(headers);
  const auto validation =
      net::http_cache_util::ValidationHeaders::MaybeCreate(headers);
  if (!validation.has_value() || validation->has_value() ||
      (load_flags & net::LOAD_DISABLE_CACHE)) {
    return std::nullopt;
  }
  net::PartialData partial;
  if (!partial.Init(headers)) {
    return std::nullopt;
  }
  net::HttpRequestHeaders moved = headers;
  moved.RemoveHeader(net::HttpRequestHeaders::kRange);
  partial.SetHeaders(moved);
  // What the network gets of a range that the cache holds none of.
  partial.RestoreHeaders(&moved);
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
