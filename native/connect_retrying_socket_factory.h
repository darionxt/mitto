#ifndef MITTO_CONNECT_RETRYING_SOCKET_FACTORY_H_
#define MITTO_CONNECT_RETRYING_SOCKET_FACTORY_H_

#include <memory>

#include "base/memory/raw_ptr.h"
#include "net/socket/client_socket_factory.h"

namespace mitto {

// Makes TCP sockets that try a connect again when the system refuses it with
// ERR_NO_BUFFER_SPACE or ERR_ADDRESS_IN_USE, up to kMaxRetries more times.
//
// On Windows 11 from 22H2, and on Windows Server 2025, //net asks Windows for a
// random source port before each connect, as Chrome there does, and Windows
// refuses about one such connect in 7,000, however few ports are in use, with
// WSAENOBUFS, or with WSAEADDRINUSE when the port it drew is still in TIME_WAIT
// towards the same server. It refuses before it sends anything. Each attempt
// opens a new socket, which draws a new random port, so a server sees one
// ordinary connect.
class ConnectRetryingSocketFactory final : public net::ClientSocketFactory {
 public:
  static constexpr int kMaxRetries = 2;

  // Wraps the sockets of `factory`, or of //net's default factory if it is
  // null. For tests, the first `refusals_for_testing` connect attempts of each
  // socket fail with ERR_NO_BUFFER_SPACE without reaching the system, each
  // second one of them after a task rather than at once.
  ConnectRetryingSocketFactory(std::unique_ptr<net::ClientSocketFactory> factory,
                               int refusals_for_testing);

  ConnectRetryingSocketFactory(const ConnectRetryingSocketFactory&) = delete;
  ConnectRetryingSocketFactory& operator=(const ConnectRetryingSocketFactory&) =
      delete;

  ~ConnectRetryingSocketFactory() override;

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
  const std::unique_ptr<net::ClientSocketFactory> owned_factory_;
  const raw_ptr<net::ClientSocketFactory> factory_;
  const int refusals_for_testing_;
};

}  // namespace mitto

#endif  // MITTO_CONNECT_RETRYING_SOCKET_FACTORY_H_
