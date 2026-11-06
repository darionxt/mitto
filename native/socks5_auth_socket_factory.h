#ifndef MITTO_SOCKS5_AUTH_SOCKET_FACTORY_H_
#define MITTO_SOCKS5_AUTH_SOCKET_FACTORY_H_

#include <memory>

#include "mitto/socks5_auth_exchange.h"
#include "net/socket/client_socket_factory.h"

namespace mitto {

// What a Session with a SOCKS5 proxy and credentials makes its sockets with.
// SOCKS5ClientSocket in //net offers only "no authentication", so a TCP socket
// from here answers it as a proxy that needs nothing would, once it has given
// the real proxy the credentials. It opens no port, and //net stays as it is.
//
// This can only work for a context with one proxy, which is what a Session has.
class Socks5AuthSocketFactory final : public net::ClientSocketFactory {
 public:
  explicit Socks5AuthSocketFactory(Socks5Credentials credentials);

  Socks5AuthSocketFactory(const Socks5AuthSocketFactory&) = delete;
  Socks5AuthSocketFactory& operator=(const Socks5AuthSocketFactory&) = delete;

  ~Socks5AuthSocketFactory() override;

  // net::ClientSocketFactory:
  std::unique_ptr<net::DatagramClientSocket> CreateDatagramClientSocket(
      net::DatagramSocket::BindType bind_type,
      net::handles::NetworkHandle target_network,
      net::NetLog* net_log,
      const net::NetLogSource& source) override;
  std::unique_ptr<net::TransportClientSocket> CreateTransportClientSocket(
      const net::AddressList& addresses,
      net::handles::NetworkHandle target_network,
      std::unique_ptr<net::SocketPerformanceWatcher> socket_performance_watcher,
      net::NetworkQualityEstimator* network_quality_estimator,
      net::NetLog* net_log,
      const net::NetLogSource& source) override;
  std::unique_ptr<net::SSLClientSocket> CreateSSLClientSocket(
      net::SSLClientContext* context,
      std::unique_ptr<net::StreamSocket> stream_socket,
      const net::HostPortPair& host_and_port,
      const net::SSLConfig& ssl_config) override;

 private:
  const Socks5Credentials credentials_;
};

}  // namespace mitto

#endif  // MITTO_SOCKS5_AUTH_SOCKET_FACTORY_H_
