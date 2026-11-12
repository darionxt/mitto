// Ported from the parts of Chromium that add noise for each host to the network
// quality estimate in the rtt and downlink client hints, with the upstream
// names kept: content/browser/client_hints/client_hints.cc, where the browser
// process works them out for a navigation, and
// third_party/blink/renderer/platform/network/network_state_notifier.cc, where
// the renderer process of a page works them out for a subresource. The source
// export leaves both out.
//
// Upstream: content/browser/client_hints/client_hints.cc
// Upstream: third_party/blink/renderer/platform/network/network_state_notifier.cc
// Upstream: third_party/blink/renderer/platform/network/network_state_notifier.h
// Upstream: third_party/blink/renderer/core/loader/frame_fetch_context.cc
// Upstream: third_party/blink/renderer/modules/netinfo/network_information.cc

#ifndef MITTO_NETWORK_QUALITY_H_
#define MITTO_NETWORK_QUALITY_H_

#include <stdint.h>

#include <string>
#include <string_view>

#include "base/time/time.h"

class GURL;

namespace net {
class SchemefulSite;
}  // namespace net

namespace mitto {

// What Chrome's network quality estimator has, which
// network::NetworkQualityTracker hands to the browser process and to each
// renderer process alike, and the salts that one run of Chrome adds noise with.
struct NetworkQuality {
  NetworkQuality();
  NetworkQuality(const NetworkQuality&);
  NetworkQuality& operator=(const NetworkQuality&);
  ~NetworkQuality();

  base::TimeDelta http_rtt;
  int32_t downstream_throughput_kbps = 0;

  // RandomizationSalt() of the browser process, from 1 to 21.
  uint8_t browser_salt = 1;

  // What the salt of the renderer process of each site comes from. Chrome
  // draws one from 1 to 20 when it starts a renderer process, and a Session
  // stands for one renderer process for each site of a page.
  std::string renderer_salt_key;
};

// The salt of the renderer process of the pages of `site`: 1 plus the first 8
// bytes of the SHA-256 of `renderer_salt_key`, a space and the serialized site,
// as a big-endian number, modulo 20.
uint8_t RendererSalt(const NetworkQuality& quality,
                     const net::SchemefulSite& site);

// Whether `name`, in any case, is rtt or downlink, which these work out.
bool IsNetworkQualityHint(std::string_view name);

// The value of the hint `name`, rtt or downlink, on a navigation's hop to
// `url`: what AddRttHeader() and AddDownlinkHeader() write in the browser
// process.
std::string GetNavigationNetworkQualityHint(const NetworkQuality& quality,
                                            std::string_view name,
                                            const GURL& url);

// What the renderer process of the pages of `page_site` gives for the host of
// `url`: NetworkStateNotifier::RoundRtt() and RoundMbps(), which a page of that
// host reads in navigator.connection, and a subresource to it sends in its
// hints.
struct RendererNetworkQuality {
  // In milliseconds.
  uint32_t rtt = 0;
  // In Mbps.
  double downlink = 0;
};
RendererNetworkQuality GetRendererNetworkQuality(
    const NetworkQuality& quality,
    const net::SchemefulSite& page_site,
    const GURL& url);

// The value of the hint `name`, rtt or downlink, on a subresource that starts
// at `url` from a page of `page_site`: what
// FrameFetchContext::AddClientHintsIfNecessary() writes in the renderer process
// of that site.
std::string GetSubresourceNetworkQualityHint(
    const NetworkQuality& quality,
    std::string_view name,
    const net::SchemefulSite& page_site,
    const GURL& url);

}  // namespace mitto

#endif  // MITTO_NETWORK_QUALITY_H_
