#include "mitto/connect_retrying_socket_factory.h"

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/location.h"
#include "base/memory/weak_ptr.h"
#include "base/notreached.h"
#include "base/task/sequenced_task_runner.h"
#include "base/types/expected.h"
#include "net/base/completion_once_callback.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/base/network_handle.h"
#include "net/log/net_log_source.h"
#include "net/socket/datagram_client_socket.h"
#include "net/socket/diff_serv_code_point.h"
#include "net/socket/socket_performance_watcher.h"
#include "net/socket/ssl_client_socket.h"
#include "net/socket/transport_client_socket.h"
#include "net/traffic_annotation/network_traffic_annotation.h"

namespace mitto {
namespace {

bool IsRefusal(int result) {
  return result == net::ERR_NO_BUFFER_SPACE ||
         result == net::ERR_ADDRESS_IN_USE;
}

class ConnectRetryingSocket final : public net::TransportClientSocket {
 public:
  ConnectRetryingSocket(std::unique_ptr<net::TransportClientSocket> socket,
                        int refusals_for_testing)
      : socket_(std::move(socket)),
        refusals_for_testing_(refusals_for_testing) {}

  ConnectRetryingSocket(const ConnectRetryingSocket&) = delete;
  ConnectRetryingSocket& operator=(const ConnectRetryingSocket&) = delete;

  ~ConnectRetryingSocket() override = default;

  int Bind(const net::IPEndPoint& local_addr) override {
    const int result = socket_->Bind(local_addr);
    if (result == net::OK) {
      bind_address_ = local_addr;
    }
    return result;
  }
  bool SetNoDelay(bool no_delay) override {
    return socket_->SetNoDelay(no_delay);
  }
  bool SetKeepAlive(bool enable, int delay_secs) override {
    return socket_->SetKeepAlive(enable, delay_secs);
  }

  void SetBeforeConnectCallback(
      const BeforeConnectCallback& before_connect_callback) override {
    socket_->SetBeforeConnectCallback(before_connect_callback);
  }
  int Connect(net::CompletionOnceCallback callback) override {
    if (socket_->IsConnected()) {
      return net::OK;
    }
    retries_left_ = ConnectRetryingSocketFactory::kMaxRetries;
    const int result = ConnectAgainWhileRefused(StartAttempt());
    if (result == net::ERR_IO_PENDING) {
      connect_callback_ = std::move(callback);
    }
    return result;
  }
  void Disconnect() override {
    socket_->Disconnect();
    bind_address_.reset();
    connect_callback_.Reset();
    weak_ptr_factory_.InvalidateWeakPtrs();
  }
  bool IsConnected() const override { return socket_->IsConnected(); }
  bool IsConnectedAndIdle() const override {
    return socket_->IsConnectedAndIdle();
  }
  int GetPeerAddress(net::IPEndPoint* address) const override {
    return socket_->GetPeerAddress(address);
  }
  int GetLocalAddress(net::IPEndPoint* address) const override {
    return socket_->GetLocalAddress(address);
  }
  const net::NetLogWithSource& NetLog() const override {
    return socket_->NetLog();
  }
  bool WasEverUsed() const override { return socket_->WasEverUsed(); }
  net::NextProto GetNegotiatedProtocol() const override {
    return socket_->GetNegotiatedProtocol();
  }
  bool GetSSLInfo(net::SSLInfo* ssl_info) override {
    return socket_->GetSSLInfo(ssl_info);
  }
  int64_t GetTotalReceivedBytes() const override {
    return socket_->GetTotalReceivedBytes();
  }
  void ApplySocketTag(const net::SocketTag& tag) override {
    socket_->ApplySocketTag(tag);
  }

  int Read(net::IOBuffer* buffer,
           int size,
           net::CompletionOnceCallback callback) override {
    return socket_->Read(buffer, size, std::move(callback));
  }
  int ReadIfReady(net::IOBuffer* buffer,
                  int size,
                  net::CompletionOnceCallback callback) override {
    return socket_->ReadIfReady(buffer, size, std::move(callback));
  }
  int CancelReadIfReady() override { return socket_->CancelReadIfReady(); }
  int Write(net::IOBuffer* buffer,
            int size,
            net::CompletionOnceCallback callback,
            const net::NetworkTrafficAnnotationTag& traffic_annotation) override {
    return socket_->Write(buffer, size, std::move(callback),
                          traffic_annotation);
  }
  int SetReceiveBufferSize(int32_t size) override {
    return socket_->SetReceiveBufferSize(size);
  }
  int SetSendBufferSize(int32_t size) override {
    return socket_->SetSendBufferSize(size);
  }

 private:
  // Takes the result of an attempt, and makes another while the system refused
  // it and retries are left. Returns the result of the last attempt, which is
  // ERR_IO_PENDING while that runs.
  int ConnectAgainWhileRefused(int result) {
    while (IsRefusal(result) && retries_left_ > 0) {
      --retries_left_;
      // A socket may connect again after Disconnect(), which also drops the
      // address that it was bound to.
      socket_->Disconnect();
      if (bind_address_) {
        result = socket_->Bind(*bind_address_);
        if (result != net::OK) {
          return result;
        }
      }
      result = StartAttempt();
    }
    return result;
  }

  int StartAttempt() {
    if (refusals_for_testing_ > 0) {
      --refusals_for_testing_;
      if (refusals_for_testing_ % 2 == 1) {
        base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE,
            base::BindOnce(&ConnectRetryingSocket::OnAttemptComplete,
                           weak_ptr_factory_.GetWeakPtr(),
                           net::ERR_NO_BUFFER_SPACE));
        return net::ERR_IO_PENDING;
      }
      return net::ERR_NO_BUFFER_SPACE;
    }
    // The socket runs no callback once it has disconnected or is gone.
    return socket_->Connect(base::BindOnce(
        &ConnectRetryingSocket::OnAttemptComplete, base::Unretained(this)));
  }

  void OnAttemptComplete(int result) {
    result = ConnectAgainWhileRefused(result);
    if (result != net::ERR_IO_PENDING) {
      std::move(connect_callback_).Run(result);
    }
  }

  const std::unique_ptr<net::TransportClientSocket> socket_;

  int refusals_for_testing_;
  std::optional<net::IPEndPoint> bind_address_;
  int retries_left_ = 0;
  net::CompletionOnceCallback connect_callback_;

  base::WeakPtrFactory<ConnectRetryingSocket> weak_ptr_factory_{this};
};


// What makes a new socket for a UDP socket to connect again with, as the first
// one was made.
using MakeDatagramSocket =
    base::RepeatingCallback<std::unique_ptr<net::DatagramClientSocket>()>;

// A UDPClientSocket connects only once, so a refused connect is tried again on
// a new socket, which gets what the caller set before it connected.
class ConnectRetryingDatagramSocket final : public net::DatagramClientSocket {
 public:
  ConnectRetryingDatagramSocket(MakeDatagramSocket make_socket,
                                int refusals_for_testing)
      : make_socket_(std::move(make_socket)),
        socket_(make_socket_.Run()),
        refusals_for_testing_(refusals_for_testing) {}

  ConnectRetryingDatagramSocket(const ConnectRetryingDatagramSocket&) = delete;
  ConnectRetryingDatagramSocket& operator=(
      const ConnectRetryingDatagramSocket&) = delete;

  ~ConnectRetryingDatagramSocket() override = default;

  int Connect(const net::IPEndPoint& address) override {
    return ConnectAgainWhileRefused(Target::kAny,
                                    net::handles::kInvalidNetworkHandle,
                                    address);
  }
  int ConnectUsingNetwork(net::handles::NetworkHandle network,
                          const net::IPEndPoint& address) override {
    return ConnectAgainWhileRefused(Target::kNetwork, network, address);
  }
  int ConnectUsingDefaultNetwork(const net::IPEndPoint& address) override {
    return ConnectAgainWhileRefused(Target::kDefaultNetwork,
                                    net::handles::kInvalidNetworkHandle,
                                    address);
  }
  // These connect at once, as UDPClientSocket's do.
  int ConnectAsync(const net::IPEndPoint& address,
                   net::CompletionOnceCallback callback) override {
    return Connect(address);
  }
  int ConnectUsingNetworkAsync(net::handles::NetworkHandle network,
                               const net::IPEndPoint& address,
                               net::CompletionOnceCallback callback) override {
    return ConnectUsingNetwork(network, address);
  }
  int ConnectUsingDefaultNetworkAsync(
      const net::IPEndPoint& address,
      net::CompletionOnceCallback callback) override {
    return ConnectUsingDefaultNetwork(address);
  }
  net::handles::NetworkHandle GetBoundNetwork() const override {
    return socket_->GetBoundNetwork();
  }
  base::expected<net::DatagramsMetadata, net::Error> ReadMultiple(
      net::IOBuffer* buffer,
      size_t buffer_size,
      size_t max_message_size,
      base::OnceCallback<void(base::expected<net::DatagramsMetadata,
                                             net::Error>)> callback) override {
    return socket_->ReadMultiple(buffer, buffer_size, max_message_size,
                                 std::move(callback));
  }
  void ApplySocketTag(const net::SocketTag& tag) override {
    socket_->ApplySocketTag(tag);
  }
  void EnableRecvOptimization() override {
    recv_optimization_ = true;
    socket_->EnableRecvOptimization();
  }
  int SetMulticastInterface(uint32_t interface_index) override {
    const int result = socket_->SetMulticastInterface(interface_index);
    if (result == net::OK) {
      multicast_interface_ = interface_index;
    }
    return result;
  }
  void SetIOSNetworkServiceType(int ios_network_service_type) override {
    ios_network_service_type_ = ios_network_service_type;
    socket_->SetIOSNetworkServiceType(ios_network_service_type);
  }
  void RegisterQuicConnectionClosePayload(base::span<uint8_t> payload) override {
    socket_->RegisterQuicConnectionClosePayload(payload);
  }
  void UnregisterQuicConnectionClosePayload() override {
    socket_->UnregisterQuicConnectionClosePayload();
  }

  void Close() override { socket_->Close(); }
  int GetPeerAddress(net::IPEndPoint* address) const override {
    return socket_->GetPeerAddress(address);
  }
  int GetLocalAddress(net::IPEndPoint* address) const override {
    return socket_->GetLocalAddress(address);
  }
  void UseNonBlockingIO() override {
    non_blocking_io_ = true;
    socket_->UseNonBlockingIO();
  }
  int SetDoNotFragment() override { return socket_->SetDoNotFragment(); }
  int SetRecvTos() override { return socket_->SetRecvTos(); }
  int SetTos(net::DiffServCodePoint dscp, net::EcnCodePoint ecn) override {
    return socket_->SetTos(dscp, ecn);
  }
  void SetMsgConfirm(bool confirm) override { socket_->SetMsgConfirm(confirm); }
  const net::NetLogWithSource& NetLog() const override {
    return socket_->NetLog();
  }
  net::DscpAndEcn GetLastTos() const override { return socket_->GetLastTos(); }

  int Read(net::IOBuffer* buffer,
           int size,
           net::CompletionOnceCallback callback) override {
    return socket_->Read(buffer, size, std::move(callback));
  }
  int Write(net::IOBuffer* buffer,
            int size,
            net::CompletionOnceCallback callback,
            const net::NetworkTrafficAnnotationTag& traffic_annotation) override {
    return socket_->Write(buffer, size, std::move(callback),
                          traffic_annotation);
  }
  int SetReceiveBufferSize(int32_t size) override {
    return socket_->SetReceiveBufferSize(size);
  }
  int SetSendBufferSize(int32_t size) override {
    return socket_->SetSendBufferSize(size);
  }

 private:
  // Which of the ways to connect the caller asked for.
  enum class Target { kAny, kNetwork, kDefaultNetwork };

  int ConnectAgainWhileRefused(Target target,
                               net::handles::NetworkHandle network,
                               const net::IPEndPoint& address) {
    int result = Attempt(target, network, address);
    for (int retries_left = ConnectRetryingSocketFactory::kMaxRetries;
         IsRefusal(result) && retries_left > 0; --retries_left) {
      // Kept, closed, so that what the caller already took from it, such as
      // its NetLog, stays valid.
      socket_->Close();
      refused_sockets_.push_back(std::exchange(socket_, make_socket_.Run()));
      if (non_blocking_io_) {
        socket_->UseNonBlockingIO();
      }
      if (recv_optimization_) {
        socket_->EnableRecvOptimization();
      }
      if (ios_network_service_type_) {
        socket_->SetIOSNetworkServiceType(*ios_network_service_type_);
      }
      if (multicast_interface_) {
        result = socket_->SetMulticastInterface(*multicast_interface_);
        if (result != net::OK) {
          return result;
        }
      }
      result = Attempt(target, network, address);
    }
    return result;
  }

  int Attempt(Target target,
              net::handles::NetworkHandle network,
              const net::IPEndPoint& address) {
    if (refusals_for_testing_ > 0) {
      --refusals_for_testing_;
      return net::ERR_NO_BUFFER_SPACE;
    }
    switch (target) {
      case Target::kAny:
        return socket_->Connect(address);
      case Target::kNetwork:
        return socket_->ConnectUsingNetwork(network, address);
      case Target::kDefaultNetwork:
        return socket_->ConnectUsingDefaultNetwork(address);
    }
    NOTREACHED();
  }

  const MakeDatagramSocket make_socket_;
  std::unique_ptr<net::DatagramClientSocket> socket_;
  std::vector<std::unique_ptr<net::DatagramClientSocket>> refused_sockets_;

  int refusals_for_testing_;
  bool non_blocking_io_ = false;
  bool recv_optimization_ = false;
  std::optional<int> ios_network_service_type_;
  std::optional<uint32_t> multicast_interface_;
};

}  // namespace

ConnectRetryingSocketFactory::ConnectRetryingSocketFactory(
    std::unique_ptr<net::ClientSocketFactory> factory,
    int refusals_for_testing)
    : owned_factory_(std::move(factory)),
      factory_(owned_factory_ ? owned_factory_.get()
                              : net::ClientSocketFactory::GetDefaultFactory()),
      refusals_for_testing_(refusals_for_testing) {}

ConnectRetryingSocketFactory::~ConnectRetryingSocketFactory() = default;

std::unique_ptr<net::DatagramClientSocket>
ConnectRetryingSocketFactory::CreateDatagramClientSocket(
    net::DatagramSocket::BindType bind_type,
    net::handles::NetworkHandle target_network,
    net::NetLog* net_log,
    const net::NetLogSource& source) {
  return std::make_unique<ConnectRetryingDatagramSocket>(
      base::BindRepeating(&net::ClientSocketFactory::CreateDatagramClientSocket,
                          base::Unretained(factory_.get()), bind_type,
                          target_network, base::Unretained(net_log), source),
      refusals_for_testing_);
}

std::unique_ptr<net::TransportClientSocket>
ConnectRetryingSocketFactory::CreateTransportClientSocket(
    const net::AddressList& addresses,
    net::handles::NetworkHandle target_network,
    std::unique_ptr<net::SocketPerformanceWatcher> socket_performance_watcher,
    net::NetworkQualityEstimator* network_quality_estimator,
    net::NetLog* net_log,
    const net::NetLogSource& source) {
  return std::make_unique<ConnectRetryingSocket>(
      factory_->CreateTransportClientSocket(
          addresses, target_network, std::move(socket_performance_watcher),
          network_quality_estimator, net_log, source),
      refusals_for_testing_);
}

std::unique_ptr<net::SSLClientSocket>
ConnectRetryingSocketFactory::CreateSSLClientSocket(
    net::SSLClientContext* context,
    std::unique_ptr<net::StreamSocket> stream_socket,
    const net::HostPortPair& host_and_port,
    const net::SSLConfig& ssl_config) {
  return factory_->CreateSSLClientSocket(context, std::move(stream_socket),
                                         host_and_port, ssl_config);
}

}  // namespace mitto
