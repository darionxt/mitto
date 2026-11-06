#include "mitto/socks5_auth_socket_factory.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/notreached.h"
#include "base/numerics/safe_conversions.h"
#include "net/base/address_list.h"
#include "net/base/completion_once_callback.h"
#include "net/base/io_buffer.h"
#include "net/base/net_errors.h"
#include "net/socket/datagram_client_socket.h"
#include "net/socket/socket_performance_watcher.h"
#include "net/socket/ssl_client_socket.h"
#include "net/socket/transport_client_socket.h"
#include "net/traffic_annotation/network_traffic_annotation.h"

namespace mitto {
namespace {

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation(
        "mitto_socks5_auth",
        "The username and password that the application named for a Session's "
        "SOCKS5 proxy, given to that proxy.");

// What SOCKS5ClientSocket sends first, and the answer it takes to mean that the
// proxy needs nothing more.
constexpr std::array<uint8_t, 3> kNoAuthenticationGreeting = {0x05, 0x01, 0x00};
constexpr std::array<uint8_t, 2> kNoAuthenticationAnswer = {0x05, 0x00};

// A TCP socket that speaks to a SOCKS5 proxy for the SOCKS5ClientSocket above it,
// as far as that one cannot, and passes on everything else.
//
// Nothing tells a factory which socket is going to the proxy. It sees an address
// that a name resolved to, and a connection that goes around the proxy may have
// the same one. The NetLog source does not say either, because a tunnel for HTTPS
// logs under the SSL connect job, as HTTPS does with no proxy. So every socket of
// the Session is one of these, and the first write tells them apart. A Session has
// one proxy, nothing but SOCKS5ClientSocket starts a connection with these three
// bytes, and neither TLS nor HTTP can. Credentials go to no other peer.
//
// It does this from that write and not from Connect(), because SOCKSConnectJob
// turns any error of Connect() into ERR_PROXY_CONNECTION_FAILED, and tries the
// proxy's next address. From here an error reaches the caller as it is, and the
// job's timer for the SOCKS5 handshake covers the exchange.
class Socks5AuthSocket final : public net::TransportClientSocket {
 public:
  Socks5AuthSocket(std::unique_ptr<net::TransportClientSocket> socket,
                   const Socks5Credentials& credentials)
      : socket_(std::move(socket)), credentials_(credentials) {}

  Socks5AuthSocket(const Socks5AuthSocket&) = delete;
  Socks5AuthSocket& operator=(const Socks5AuthSocket&) = delete;

  ~Socks5AuthSocket() override = default;

  // net::TransportClientSocket:
  int Bind(const net::IPEndPoint& local_addr) override {
    return socket_->Bind(local_addr);
  }
  bool SetNoDelay(bool no_delay) override {
    return socket_->SetNoDelay(no_delay);
  }
  bool SetKeepAlive(bool enable, int delay_secs) override {
    return socket_->SetKeepAlive(enable, delay_secs);
  }

  // net::StreamSocket:
  void SetBeforeConnectCallback(
      const BeforeConnectCallback& before_connect_callback) override {
    socket_->SetBeforeConnectCallback(before_connect_callback);
  }
  int Connect(net::CompletionOnceCallback callback) override {
    return socket_->Connect(std::move(callback));
  }
  void Disconnect() override {
    socket_->Disconnect();
    // Disconnecting ended any I/O of the exchange, and Connect() may follow.
    exchange_.reset();
    next_state_ = State::kNone;
    write_buffer_.reset();
    read_buffer_.reset();
    greeting_callback_.Reset();
    wrote_before_ = false;
    answer_bytes_left_ = 0;
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

  // net::Socket:
  int Read(net::IOBuffer* buffer,
           int size,
           net::CompletionOnceCallback callback) override {
    if (answer_bytes_left_ == 0) {
      return socket_->Read(buffer, size, std::move(callback));
    }
    const size_t count =
        std::min(answer_bytes_left_, base::checked_cast<size_t>(size));
    buffer->first(count).copy_from(
        base::span(kNoAuthenticationAnswer).last(answer_bytes_left_).first(count));
    answer_bytes_left_ -= count;
    return base::checked_cast<int>(count);
  }
  int ReadIfReady(net::IOBuffer* buffer,
                  int size,
                  net::CompletionOnceCallback callback) override {
    // A TLS socket reads this way, and only one with no proxy sits right on top.
    return socket_->ReadIfReady(buffer, size, std::move(callback));
  }
  int CancelReadIfReady() override { return socket_->CancelReadIfReady(); }
  int Write(net::IOBuffer* buffer,
            int size,
            net::CompletionOnceCallback callback,
            const net::NetworkTrafficAnnotationTag& traffic_annotation) override {
    if (std::exchange(wrote_before_, true) ||
        !std::ranges::equal(buffer->first(base::checked_cast<size_t>(size)),
                            kNoAuthenticationGreeting)) {
      return socket_->Write(buffer, size, std::move(callback),
                            traffic_annotation);
    }
    exchange_.emplace(credentials_);
    StartMessage();
    next_state_ = State::kWrite;
    const int result = DoLoop(net::OK);
    if (result == net::ERR_IO_PENDING) {
      greeting_callback_ = std::move(callback);
      return result;
    }
    return OnExchangeOver(result);
  }
  int SetReceiveBufferSize(int32_t size) override {
    return socket_->SetReceiveBufferSize(size);
  }
  int SetSendBufferSize(int32_t size) override {
    return socket_->SetSendBufferSize(size);
  }

 private:
  enum class State {
    kNone,
    kWrite,
    kWriteComplete,
    kRead,
    kReadComplete,
  };

  int DoLoop(int result) {
    do {
      switch (std::exchange(next_state_, State::kNone)) {
        case State::kWrite:
          result = DoWrite();
          break;
        case State::kWriteComplete:
          result = DoWriteComplete(result);
          break;
        case State::kRead:
          result = DoRead();
          break;
        case State::kReadComplete:
          result = DoReadComplete(result);
          break;
        case State::kNone:
          NOTREACHED();
      }
    } while (result != net::ERR_IO_PENDING && next_state_ != State::kNone);
    return result;
  }

  int DoWrite() {
    next_state_ = State::kWriteComplete;
    return socket_->Write(write_buffer_.get(), write_buffer_->BytesRemaining(),
                          base::BindOnce(&Socks5AuthSocket::OnIOComplete,
                                         base::Unretained(this)),
                          kTrafficAnnotation);
  }

  int DoWriteComplete(int result) {
    if (result < 0) {
      return result;
    }
    write_buffer_->DidConsume(result);
    next_state_ =
        write_buffer_->BytesRemaining() > 0 ? State::kWrite : State::kRead;
    return net::OK;
  }

  int DoRead() {
    read_buffer_ =
        base::MakeRefCounted<net::IOBufferWithSize>(exchange_->bytes_wanted());
    next_state_ = State::kReadComplete;
    return socket_->Read(read_buffer_.get(), read_buffer_->size(),
                         base::BindOnce(&Socks5AuthSocket::OnIOComplete,
                                        base::Unretained(this)));
  }

  int DoReadComplete(int result) {
    if (result < 0) {
      return result;
    }
    if (result == 0) {
      return net::ERR_SOCKS_CONNECTION_FAILED;
    }
    const int exchange_result = exchange_->OnBytesRead(
        read_buffer_->first(base::checked_cast<size_t>(result)));
    if (exchange_result != net::ERR_IO_PENDING) {
      return exchange_result;
    }
    next_state_ = StartMessage() ? State::kWrite : State::kRead;
    return net::OK;
  }

  // Returns whether there is something to send.
  bool StartMessage() {
    std::vector<uint8_t> message = exchange_->TakeMessage();
    if (message.empty()) {
      return false;
    }
    const size_t size = message.size();
    write_buffer_ = base::MakeRefCounted<net::DrainableIOBuffer>(
        base::MakeRefCounted<net::VectorIOBuffer>(std::move(message)), size);
    return true;
  }

  void OnIOComplete(int result) {
    result = DoLoop(result);
    if (result != net::ERR_IO_PENDING) {
      // This may delete the socket, so nothing may touch it afterwards.
      std::move(greeting_callback_).Run(OnExchangeOver(result));
    }
  }

  // Returns what the write of the greeting comes to: all of it, once a request
  // can follow, and otherwise the error.
  int OnExchangeOver(int result) {
    exchange_.reset();
    write_buffer_.reset();
    read_buffer_.reset();
    if (result != net::OK) {
      return result;
    }
    answer_bytes_left_ = kNoAuthenticationAnswer.size();
    return base::checked_cast<int>(kNoAuthenticationGreeting.size());
  }

  const std::unique_ptr<net::TransportClientSocket> socket_;
  const Socks5Credentials credentials_;

  // Whether a write has come, which is the one chance of being a greeting.
  bool wrote_before_ = false;
  // Only during the exchange, which the write of the greeting stands for.
  std::optional<Socks5AuthExchange> exchange_;
  State next_state_ = State::kNone;
  scoped_refptr<net::DrainableIOBuffer> write_buffer_;
  scoped_refptr<net::IOBufferWithSize> read_buffer_;
  net::CompletionOnceCallback greeting_callback_;
  // How much of the answer to the greeting is left to hand over.
  size_t answer_bytes_left_ = 0;
};

}  // namespace

Socks5AuthSocketFactory::Socks5AuthSocketFactory(Socks5Credentials credentials)
    : credentials_(std::move(credentials)) {}

Socks5AuthSocketFactory::~Socks5AuthSocketFactory() = default;

std::unique_ptr<net::DatagramClientSocket>
Socks5AuthSocketFactory::CreateDatagramClientSocket(
    net::DatagramSocket::BindType bind_type,
    net::handles::NetworkHandle target_network,
    net::NetLog* net_log,
    const net::NetLogSource& source) {
  return net::ClientSocketFactory::GetDefaultFactory()
      ->CreateDatagramClientSocket(bind_type, target_network, net_log, source);
}

std::unique_ptr<net::TransportClientSocket>
Socks5AuthSocketFactory::CreateTransportClientSocket(
    const net::AddressList& addresses,
    net::handles::NetworkHandle target_network,
    std::unique_ptr<net::SocketPerformanceWatcher> socket_performance_watcher,
    net::NetworkQualityEstimator* network_quality_estimator,
    net::NetLog* net_log,
    const net::NetLogSource& source) {
  return std::make_unique<Socks5AuthSocket>(
      net::ClientSocketFactory::GetDefaultFactory()
          ->CreateTransportClientSocket(addresses, target_network,
                                        std::move(socket_performance_watcher),
                                        network_quality_estimator, net_log,
                                        source),
      credentials_);
}

std::unique_ptr<net::SSLClientSocket>
Socks5AuthSocketFactory::CreateSSLClientSocket(
    net::SSLClientContext* context,
    std::unique_ptr<net::StreamSocket> stream_socket,
    const net::HostPortPair& host_and_port,
    const net::SSLConfig& ssl_config) {
  return net::ClientSocketFactory::GetDefaultFactory()->CreateSSLClientSocket(
      context, std::move(stream_socket), host_and_port, ssl_config);
}

}  // namespace mitto
