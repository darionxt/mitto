#include "mitto/socks5_auth_exchange.h"

#include <string_view>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/numerics/safe_conversions.h"
#include "net/base/net_errors.h"

namespace mitto {
namespace {

constexpr uint8_t kSocksVersion = 0x05;
constexpr uint8_t kNoAuthentication = 0x00;
constexpr uint8_t kUsernameAndPassword = 0x02;
constexpr uint8_t kUsernameAndPasswordVersion = 0x01;
constexpr uint8_t kStatusSucceeded = 0x00;

void AppendWithSize(std::vector<uint8_t>& message, std::string_view field) {
  CHECK_LE(field.size(), kMaxSocks5CredentialSize);
  message.push_back(base::checked_cast<uint8_t>(field.size()));
  message.insert(message.end(), field.begin(), field.end());
}

}  // namespace

Socks5AuthExchange::Socks5AuthExchange(const Socks5Credentials& credentials)
    : message_({kSocksVersion, 2, kNoAuthentication, kUsernameAndPassword}) {
  credentials_message_.push_back(kUsernameAndPasswordVersion);
  AppendWithSize(credentials_message_, credentials.username);
  AppendWithSize(credentials_message_, credentials.password);
}

Socks5AuthExchange::~Socks5AuthExchange() = default;

std::vector<uint8_t> Socks5AuthExchange::TakeMessage() {
  return std::exchange(message_, {});
}

size_t Socks5AuthExchange::bytes_wanted() const {
  return step_ == Step::kOver ? 0 : answer_.size() - answer_size_;
}

int Socks5AuthExchange::OnBytesRead(base::span<const uint8_t> bytes) {
  CHECK(!bytes.empty());
  CHECK_LE(bytes.size(), bytes_wanted());
  base::span(answer_).subspan(answer_size_).copy_prefix_from(bytes);
  answer_size_ += bytes.size();
  if (answer_size_ < answer_.size()) {
    return net::ERR_IO_PENDING;
  }
  answer_size_ = 0;
  return step_ == Step::kMethod ? OnMethodAnswer() : OnStatusAnswer();
}

int Socks5AuthExchange::OnMethodAnswer() {
  if (answer_[0] != kSocksVersion) {
    step_ = Step::kOver;
    return net::ERR_SOCKS_CONNECTION_FAILED;
  }
  switch (answer_[1]) {
    case kNoAuthentication:
      step_ = Step::kOver;
      return net::OK;
    case kUsernameAndPassword:
      step_ = Step::kStatus;
      message_ = std::move(credentials_message_);
      return net::ERR_IO_PENDING;
    default:
      step_ = Step::kOver;
      return net::ERR_PROXY_AUTH_UNSUPPORTED;
  }
}

int Socks5AuthExchange::OnStatusAnswer() {
  step_ = Step::kOver;
  if (answer_[0] != kUsernameAndPasswordVersion) {
    return net::ERR_SOCKS_CONNECTION_FAILED;
  }
  return answer_[1] == kStatusSucceeded ? net::OK
                                        : net::ERR_PROXY_AUTH_REQUESTED;
}

}  // namespace mitto
