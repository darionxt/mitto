// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file of Chromium's source tree.
//
// See the header for where this comes from and what is left out.

#include "mitto/ssl_config_service_mojo.h"

#include <stdint.h>

#include <utility>
#include <vector>

#include "base/containers/extend.h"
#include "base/feature_list.h"
#include "net/base/features.h"
#include "net/cert/internal/trust_store_chrome.h"
#include "net/cert/x509_util.h"
#include "net/ssl/ssl_config.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"

namespace mitto {

net::SSLContextConfig GetNewSSLConfig() {
  net::SSLContextConfig config;

  // The next four are what network::mojom::SSLConfig holds when no preference
  // or policy sets them. They are repeated, like the values in
  // ConfigureHttp2Params(), so that a new default in //net cannot change what
  // mitto sends without a Parity Check failing.
  config.version_min = net::SSL_PROTOCOL_VERSION_TLS1_2;
  config.version_max = net::SSL_PROTOCOL_VERSION_TLS1_3;
  config.disabled_cipher_suites = {};
  config.tls13_cipher_prefer_aes_256 = false;

  // What named_groups_preset's kDefault means, in this order: the post-quantum
  // group first, and a key share for it and for X25519 only.
  config.supported_named_groups = {
      {.group_id = SSL_GROUP_X25519_MLKEM768, .send_key_share = true},
      {.group_id = SSL_GROUP_X25519, .send_key_share = true},
      {.group_id = SSL_GROUP_SECP256R1, .send_key_share = false},
      {.group_id = SSL_GROUP_SECP384R1, .send_key_share = false},
  };

  // As in SSLConfigServiceManager's constructor. This is the same root store
  // that //net verifies certificates with, so nothing has to be carried for it.
  // In Chrome the PKI metadata component replaces this list later, when it
  // brings a newer root store and when it brings Merkle Tree Certificate
  // landmarks, which also set time_bound_trust_anchor_ids. mitto has no
  // component, so it stays with the Engine Release's list.
  if (base::FeatureList::IsEnabled(net::features::kTLSTrustAnchorIDs)) {
    std::vector<std::vector<uint8_t>> trust_anchor_ids;
    if (base::FeatureList::IsEnabled(net::features::kNonMtcTrustAnchorIDs)) {
      base::Extend(
          trust_anchor_ids,
          net::TrustStoreChrome::GetTrustAnchorIDsFromCompiledInRootStore());
    }
    if (base::FeatureList::IsEnabled(net::features::kVerifyMTCs)) {
      base::Extend(
          trust_anchor_ids,
          net::TrustStoreChrome::GetTrustedMtcCaIDsFromCompiledInRootStore());
    }
    config.trust_anchor_ids =
        net::x509_util::EncodeTlsRequestedTrustAnchorIDList(
            std::move(trust_anchor_ids));
  }

  return config;
}

SSLConfigServiceMojo::SSLConfigServiceMojo(net::SSLContextConfig initial_config)
    : ssl_context_config_(std::move(initial_config)) {}

SSLConfigServiceMojo::~SSLConfigServiceMojo() = default;

net::SSLContextConfig SSLConfigServiceMojo::GetSSLContextConfig() {
  return ssl_context_config_;
}

net::EchMode SSLConfigServiceMojo::GetEchMode(std::string_view hostname) const {
  // Upstream's ech_enabled is true without a policy. This mode sends ECH GREASE
  // when the host has no ECH configuration in DNS, and uses one when it has.
  return net::EchMode::kOpportunistic;
}

bool SSLConfigServiceMojo::CanShareConnectionWithClientCerts(
    std::string_view hostname) const {
  // Upstream's client_cert_pooling_policy is empty without a policy.
  return false;
}

}  // namespace mitto
