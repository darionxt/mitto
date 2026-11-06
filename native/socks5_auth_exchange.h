#ifndef MITTO_SOCKS5_AUTH_EXCHANGE_H_
#define MITTO_SOCKS5_AUTH_EXCHANGE_H_

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <string>
#include <vector>

#include "base/containers/span.h"

namespace mitto {

// RFC 1929 gives the length of a username or a password one byte.
inline constexpr size_t kMaxSocks5CredentialSize = 255;

// What a SOCKS5 proxy's URL holds. RFC 1929 sends bytes, and names no encoding.
// Neither may be longer than kMaxSocks5CredentialSize.
struct Socks5Credentials {
  std::string username;
  std::string password;
};

// The client's side of what comes before a request to a SOCKS5 proxy: RFC 1928's
// choice of a method and RFC 1929's username and password, with no socket. The
// proxy's answers come from outside the library, so anything may be in them.
//
// It offers both "no authentication" and "username and password", so that a
// proxy that needs no credentials never gets them.
class Socks5AuthExchange {
 public:
  explicit Socks5AuthExchange(const Socks5Credentials& credentials);

  Socks5AuthExchange(const Socks5AuthExchange&) = delete;
  Socks5AuthExchange& operator=(const Socks5AuthExchange&) = delete;

  ~Socks5AuthExchange();

  // Returns what to send the proxy before reading on, which is empty if there is
  // nothing. A call hands it over, so the next one returns nothing.
  std::vector<uint8_t> TakeMessage();

  // How many bytes the answer still lacks, and so how many to read, so that
  // nothing that follows it is read. It is 0 once the exchange is over.
  size_t bytes_wanted() const;

  // Takes some of the proxy's answer, which is at least one byte and at most
  // bytes_wanted(). Returns ERR_IO_PENDING if the exchange goes on, OK if a
  // request can follow, and otherwise the net error that ended it:
  // ERR_PROXY_AUTH_REQUESTED if the proxy refused the credentials,
  // ERR_PROXY_AUTH_UNSUPPORTED if it wants neither method and
  // ERR_SOCKS_CONNECTION_FAILED if it does not speak the protocol.
  int OnBytesRead(base::span<const uint8_t> bytes);

 private:
  enum class Step {
    kMethod,
    kStatus,
    kOver,
  };

  int OnMethodAnswer();
  int OnStatusAnswer();

  Step step_ = Step::kMethod;
  std::vector<uint8_t> message_;
  std::vector<uint8_t> credentials_message_;
  // Each answer is two bytes long, and a socket may deliver them apart.
  std::array<uint8_t, 2> answer_ = {};
  size_t answer_size_ = 0;
};

}  // namespace mitto

#endif  // MITTO_SOCKS5_AUTH_EXCHANGE_H_
