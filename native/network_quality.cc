#include "mitto/network_quality.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <string>
#include <string_view>

#include "base/check.h"
#include "base/containers/span.h"
#include "base/numerics/byte_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "crypto/hash.h"
#include "mitto/string_hasher.h"
#include "net/base/schemeful_site.h"
#include "url/gurl.h"

namespace mitto {
namespace {

constexpr std::string_view kRttHint = "rtt";
constexpr std::string_view kDownlinkHint = "downlink";

// NetworkStateNotifier draws its salt with RandIntInclusive(1, 20).
constexpr uint64_t kRendererSalts = 20;

// content/browser/client_hints/client_hints.cc, where the salt is drawn once
// for the browser process. A Session holds its own.

constexpr size_t kMaxRandomNumbers = 21;

// libc++ hashes a string with CityHash64 where size_t has 64 bits, as it has on
// every Host and in a 64-bit Chrome on Windows. Chrome builds its own libc++
// with the same ABI version on every platform, and the native core is built
// with it too, so this std::hash is the one that Chrome on Windows calls.
static_assert(sizeof(size_t) == 8);

double GetRandomMultiplier(const std::string& host,
                           uint8_t randomization_salt) {
  // A function of the host, so that one origin cannot learn another's value,
  // and of a salt that only this run of Chrome knows, so that an origin cannot
  // take the noise out. Upstream cuts the sum to 32 bits.
  const unsigned hash = static_cast<unsigned>(std::hash<std::string>{}(host) +
                                              randomization_salt);
  return 0.9 + static_cast<double>((hash % kMaxRandomNumbers)) * 0.01;
}

int64_t RoundRtt(const std::string& host,
                 base::TimeDelta rtt,
                 uint8_t randomization_salt) {
  constexpr base::TimeDelta kMaxRtt = base::Seconds(3);
  constexpr base::TimeDelta kGranularity = base::Milliseconds(50);

  // TimeDelta holds whole microseconds, so the product drops any fraction of
  // one.
  const base::TimeDelta modified_rtt =
      std::min(rtt * GetRandomMultiplier(host, randomization_salt), kMaxRtt);
  return modified_rtt.RoundToMultiple(kGranularity).InMilliseconds();
}

double RoundKbpsToMbps(const std::string& host,
                       int32_t downlink_kbps,
                       uint8_t randomization_salt) {
  constexpr size_t kGranularityKbps = 50;
  constexpr double kMaxDownlinkKbps = 10.0 * 1000;

  double randomized_downlink_kbps = downlink_kbps;
  randomized_downlink_kbps *= GetRandomMultiplier(host, randomization_salt);
  randomized_downlink_kbps =
      std::min(randomized_downlink_kbps, kMaxDownlinkKbps);
  const double downlink_kbps_rounded =
      std::round(randomized_downlink_kbps / kGranularityKbps) *
      kGranularityKbps;
  return downlink_kbps_rounded / 1000;
}

// What SetHeaderToInt() writes.
std::string IntToHeaderValue(double value) {
  return base::NumberToString(std::round(value));
}

// What SetHeaderToDouble() writes.
std::string DoubleToSpecCompliantString(double value) {
  std::string result = base::NumberToString(value);
  if (value >= 1.0 || result.at(0) != '.') {
    return result;
  }
  return base::StrCat({"0", result});
}

// third_party/blink/renderer/platform/network/network_state_notifier.cc, for
// one renderer process, which draws its salt when it starts.
class NetworkStateNotifier {
 public:
  explicit NetworkStateNotifier(uint8_t randomization_salt)
      : randomization_salt_(randomization_salt) {}

  uint32_t RoundRtt(const std::string& host, base::TimeDelta rtt) const {
    constexpr base::TimeDelta kMaxRtt = base::Seconds(3);
    constexpr base::TimeDelta kGranularity = base::Milliseconds(50);

    const base::TimeDelta modified_rtt =
        std::min(rtt * GetRandomMultiplier(host), kMaxRtt);
    return static_cast<uint32_t>(
        modified_rtt.RoundToMultiple(kGranularity).InMilliseconds());
  }

  double RoundMbps(const std::string& host, double downlink_mbps) const {
    constexpr size_t kBucketSize = 50;
    constexpr double kMaxDownlinkKbps = 10.0 * 1000;

    double downlink_kbps = downlink_mbps * 1000;
    downlink_kbps *= GetRandomMultiplier(host);
    downlink_kbps = std::min(downlink_kbps, kMaxDownlinkKbps);
    const double downlink_kbps_rounded =
        std::round(downlink_kbps / kBucketSize) * kBucketSize;
    return downlink_kbps_rounded / 1000;
  }

 private:
  // Upstream gives 1 for a null host, which a URL of a request never has.
  double GetRandomMultiplier(const std::string& host) const {
    const uint32_t hash = HashString24(host) + RandomizationSalt();
    return 0.9 + static_cast<double>((hash % 21)) * 0.01;
  }

  uint8_t RandomizationSalt() const { return randomization_salt_; }

  const uint8_t randomization_salt_;
};

}  // namespace

NetworkQuality::NetworkQuality() = default;
NetworkQuality::NetworkQuality(const NetworkQuality&) = default;
NetworkQuality& NetworkQuality::operator=(const NetworkQuality&) = default;
NetworkQuality::~NetworkQuality() = default;

uint8_t RendererSalt(const NetworkQuality& quality,
                     const net::SchemefulSite& site) {
  const std::array<uint8_t, crypto::hash::kSha256Size> digest =
      crypto::hash::Sha256(
          base::StrCat({quality.renderer_salt_key, " ", site.Serialize()}));
  return static_cast<uint8_t>(
      1 +
      base::U64FromBigEndian(base::span(digest).first<8u>()) % kRendererSalts);
}

bool IsNetworkQualityHint(std::string_view name) {
  return base::EqualsCaseInsensitiveASCII(name, kRttHint) ||
         base::EqualsCaseInsensitiveASCII(name, kDownlinkHint);
}

std::string GetNavigationNetworkQualityHint(const NetworkQuality& quality,
                                            std::string_view name,
                                            const GURL& url) {
  const std::string host = url.GetHost();
  if (base::EqualsCaseInsensitiveASCII(name, kRttHint)) {
    return IntToHeaderValue(
        RoundRtt(host, quality.http_rtt, quality.browser_salt));
  }
  CHECK(base::EqualsCaseInsensitiveASCII(name, kDownlinkHint));
  return DoubleToSpecCompliantString(RoundKbpsToMbps(
      host, quality.downstream_throughput_kbps, quality.browser_salt));
}

std::string GetSubresourceNetworkQualityHint(
    const NetworkQuality& quality,
    std::string_view name,
    const net::SchemefulSite& page_site,
    const GURL& url) {
  const NetworkStateNotifier notifier(RendererSalt(quality, page_site));
  // Blink takes the host of its KURL, which comes from the same canonical form
  // as a GURL's.
  const std::string host = url.GetHost();
  if (base::EqualsCaseInsensitiveASCII(name, kRttHint)) {
    return base::NumberToString(notifier.RoundRtt(host, quality.http_rtt));
  }
  CHECK(base::EqualsCaseInsensitiveASCII(name, kDownlinkHint));
  // SetNetworkQuality() keeps the throughput in Mbps. String::Number() writes
  // 6 significant digits, which for a multiple of 0.05 from 0 to 10 is the
  // shortest decimal, as here.
  const double downlink_mbps =
      static_cast<double>(quality.downstream_throughput_kbps) / 1000;
  return base::NumberToString(notifier.RoundMbps(host, downlink_mbps));
}

}  // namespace mitto
