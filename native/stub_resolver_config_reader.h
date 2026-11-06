#ifndef MITTO_STUB_RESOLVER_CONFIG_READER_H_
#define MITTO_STUB_RESOLVER_CONFIG_READER_H_

#include <optional>

#include "net/base/ip_endpoint.h"

namespace net {
class HostResolverManager;
}  // namespace net

namespace mitto {

// Sets `host_resolver_manager` to what Chrome comes to with its default
// preferences. With `dns_nameserver`, it asks that server alone and takes
// nothing from the system's settings, so that a test can answer the queries.
void ConfigureStubHostResolver(
    net::HostResolverManager& host_resolver_manager,
    const std::optional<net::IPEndPoint>& dns_nameserver);

}  // namespace mitto

#endif  // MITTO_STUB_RESOLVER_CONFIG_READER_H_
