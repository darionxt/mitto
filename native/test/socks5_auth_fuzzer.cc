// Feeds any bytes to the code that reads what a SOCKS5 proxy answers before a
// request. They come from outside the library, over the network.
//
// The first byte of the input says how a socket cuts up the rest, which is the
// proxy's answers.

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "mitto/socks5_auth_exchange.h"
#include "net/base/net_errors.h"

namespace {

constexpr std::array<uint8_t, 4> kGreeting = {0x05, 0x02, 0x00, 0x02};
constexpr std::array<uint8_t, 2> kAskForCredentials = {0x05, 0x02};
constexpr std::array<uint8_t, 2> kNoCredentialsNeeded = {0x05, 0x00};
constexpr std::array<uint8_t, 2> kCredentialsAccepted = {0x01, 0x00};
constexpr std::array<uint8_t, 8> kCredentials = {0x01, 0x02, 'm',  'e',
                                                 0x03, 'p',  'a',  's'};

bool StartsWith(base::span<const uint8_t> bytes,
                base::span<const uint8_t> prefix) {
  return bytes.size() >= prefix.size() &&
         std::ranges::equal(bytes.first(prefix.size()), prefix);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // SAFETY: libFuzzer says `data` holds `size` bytes.
  base::span<const uint8_t> input = UNSAFE_BUFFERS(base::span(data, size));
  if (input.empty()) {
    return 0;
  }
  const bool one_byte_at_a_time = input.take_first_elem() % 2 == 1;
  const base::span<const uint8_t> answers = input;

  mitto::Socks5AuthExchange exchange({"me", "pas"});
  CHECK(std::ranges::equal(exchange.TakeMessage(), kGreeting));
  CHECK(exchange.TakeMessage().empty());

  int result = net::ERR_IO_PENDING;
  while (result == net::ERR_IO_PENDING && !input.empty()) {
    const size_t wanted = exchange.bytes_wanted();
    CHECK_GE(wanted, 1u);
    CHECK_LE(wanted, 2u);
    result = exchange.OnBytesRead(input.take_first(
        std::min(input.size(), one_byte_at_a_time ? size_t{1} : wanted)));

    // The credentials go only to a proxy that asked for them, and only once.
    const std::vector<uint8_t> message = exchange.TakeMessage();
    if (!message.empty()) {
      CHECK(StartsWith(answers, kAskForCredentials));
      CHECK_EQ(answers.size() - input.size(), 2u);
      CHECK(std::ranges::equal(message, kCredentials));
    }
  }
  if (result == net::ERR_IO_PENDING) {
    return 0;
  }

  // It is over, and it reads nothing beyond an answer that ended it.
  CHECK_EQ(exchange.bytes_wanted(), 0u);
  if (result == net::OK) {
    CHECK(StartsWith(answers, kNoCredentialsNeeded) ||
          (StartsWith(answers, kAskForCredentials) &&
           StartsWith(answers.subspan(2u), kCredentialsAccepted)));
  }
  return 0;
}
