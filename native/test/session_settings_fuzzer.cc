// Feeds any bytes to the code that reads a Session's settings. They come from
// outside the library, through mitto_session_create().

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <string_view>

#include "base/check.h"
#include "base/compiler_specific.h"
#include "mitto/session_settings.h"
#include "net/base/host_mapping_rules.h"
#include "net/base/host_port_pair.h"
#include "net/http/http_request_headers.h"
#include "url/scheme_host_port.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // SAFETY: libFuzzer says `data` holds `size` bytes.
  const std::string_view json = UNSAFE_BUFFERS(
      std::string_view(reinterpret_cast<const char*>(data), size));
  const std::optional<mitto::SessionSettings> settings =
      mitto::ParseSessionSettings(json);
  if (!settings) {
    return 0;
  }

  // What a Session relies on in settings that were accepted. //net CHECKs the
  // value of each header it adds, an invalid origin would make it accept
  // unknown roots for every host, and the verifier uses each anchor without
  // checking it.
  net::HttpRequestHeaders headers;
  headers.SetHeader(net::HttpRequestHeaders::kAcceptLanguage,
                    settings->accept_language);
  for (const url::SchemeHostPort& origin : settings->origins_to_force_quic_on) {
    CHECK(origin.IsValid());
  }
  for (const auto& anchor : settings->additional_trust_anchors) {
    CHECK(anchor);
  }
  net::HostMappingRules rules;
  rules.SetRulesFromString(settings->host_resolver_rules);
  net::HostPortPair host_port("capture.test", 443);
  rules.RewriteHost(&host_port);
  return 0;
}
