#ifndef MITTO_RANGE_AFTER_CACHE_H_
#define MITTO_RANGE_AFTER_CACHE_H_

#include <optional>
#include <string_view>

#include "net/base/network_delegate_impl.h"
#include "net/http/http_request_headers.h"

namespace mitto {

// The lines that a request of `method` and `load_flags` with `headers` goes
// out with through Chrome's HTTP cache, when that cache moves its Range line:
// HttpCache::Transaction::SetRequest() takes it off a GET of one valid range
// that it may cache, and PartialData::RestoreHeaders() puts it back after the
// others, as HttpByteRange writes it, ahead of what HTTP/2 and HTTP/3 add, such
// as `priority`. Else nullopt, and the lines stay as they are.
//
// Upstream: net/http/http_cache_transaction.cc
std::optional<net::HttpRequestHeaders> HeadersAfterHttpCache(
    std::string_view method,
    int load_flags,
    const net::HttpRequestHeaders& headers);

// The network delegate of a Session without an HTTP cache, which Chrome always
// has, so that a request's Range line goes where Chrome's cache puts it, after
// the lines that //net adds, such as Accept-Language and Cookie. A Session with
// a cache has //net's own, which moves it, and the default delegate.
class NoHttpCacheNetworkDelegate final : public net::NetworkDelegateImpl {
 public:
  NoHttpCacheNetworkDelegate();
  NoHttpCacheNetworkDelegate(const NoHttpCacheNetworkDelegate&) = delete;
  NoHttpCacheNetworkDelegate& operator=(const NoHttpCacheNetworkDelegate&) =
      delete;
  ~NoHttpCacheNetworkDelegate() override;

 private:
  // net::NetworkDelegateImpl:
  int OnBeforeStartTransaction(
      net::URLRequest* request,
      const net::HttpRequestHeaders& headers,
      OnBeforeStartTransactionCallback callback) override;
};

}  // namespace mitto

#endif  // MITTO_RANGE_AFTER_CACHE_H_
