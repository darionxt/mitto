#include "mitto/feature_list.h"

#include <string_view>
#include <vector>

#include "base/feature_list.h"
#include "base/strings/string_util.h"
#include "net/base/features.h"
#include "net/http/structured_headers.h"

namespace mitto {

void InitFeatureList() {
  // These are the features' own name fields, and not strings, because //base
  // ignores a name it does not know. A feature that upstream renames or removes
  // stops the build instead of quietly going back to its default.
  const std::vector<std::string_view> enable_features = {
      // The features in //net that the Chrome behind the Baselines has on, as
      // chrome.txt beside them shows. A source build has the first two off. No
      // Parity Check can see these four, because they are about what a server
      // sends back and the HTTP cache, which a Session does not have yet.
      // ConfigureQuicHints is the one more that chrome.txt has on. It is stated
      // per Session instead, by the function of that name.
      net::features::kLogicalClearHttpCache.name,
      net::structured_headers::kStructuredHeadersInRust.name,
      net::features::kHttpCacheZstdCompression.name,
      net::features::kHttpCacheZstdDecompression.name,

      // The features that decide what the ClientHello holds. Each one defaults
      // to this state in //net. No study in the variations seed behind the
      // Baselines names any of them, so they are not field trials today. They
      // are stated so that a new default in //net cannot change what mitto
      // sends without a Parity Check failing.
      net::features::kAlpsForHttp2.name,
      net::features::kUseNewAlpsCodepointHttp2.name,
      net::features::kTlsGreaseSigalgs.name,
      net::features::kTLSTrustAnchorIDs.name,
      net::features::kNonMtcTrustAnchorIDs.name,

      // The same for QUIC's handshake. QuicSessionPool adds this feature's
      // parameter quic_options, whose default is the ORIG in
      // google_connection_options. A study changes that parameter for about 3%
      // of Chrome installations. See docs/baselines.md.
      net::features::kTryQuicByDefault.name,
  };
  const std::vector<std::string_view> disable_features = {
      // More that decide what the ClientHello holds, in the same way.
      net::features::kVerifyMTCs.name,
      net::features::kAddTLSServerHandshakePadding.name,
      net::features::kIgnoreIpMatching.name,

      // No Parity Check can see this one, because the capture server gives out
      // no session tickets, and early data needs a resumed connection.
      net::features::kEnableTLS13EarlyData.name,
  };
  base::FeatureList::InitInstance(base::JoinString(enable_features, ","),
                                  base::JoinString(disable_features, ","));
}

}  // namespace mitto
