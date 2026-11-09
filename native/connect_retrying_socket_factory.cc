#include "mitto/connect_retrying_socket_factory.h"

#include <stdint.h>

#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "net/base/completion_once_callback.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/socket/datagram_client_socket.h"
#include "net/socket/socket_performance_watcher.h"
#include "net/socket/ssl_client_socket.h"
#include "net/socket/transport_client_socket.h"

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
  return factory_->CreateDatagramClientSocket(bind_type, target_network,
                                              net_log, source);
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
