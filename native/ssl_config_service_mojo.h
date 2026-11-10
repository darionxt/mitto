// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file of Chromium's source tree.
//
// Ported from services/network/ssl_config_service_mojo.h, whose build target
// brings in Mojo and the rest of the network service. The name stays so that it
// compares with upstream, but no Mojo is left in it: a Session's configuration
// is given once and never changes. GetNewSSLConfig() stands in for what reaches
// that class from chrome/browser/ssl/ssl_config_service_manager.cc through
// services/network/ssl_config_type_converter.cc. What that configuration says
// about verifying certificates is left out, because it is not in the handshake.
//
// Upstream: services/network/ssl_config_service_mojo.h
// Upstream: chrome/browser/ssl/ssl_config_service_manager.cc
// Upstream: services/network/ssl_config_type_converter.cc

#ifndef MITTO_SSL_CONFIG_SERVICE_MOJO_H_
#define MITTO_SSL_CONFIG_SERVICE_MOJO_H_

#include <string_view>

#include "net/base/ech_mode.h"
#include "net/ssl/ssl_config_service.h"

namespace mitto {

// Returns the configuration Chrome's network service gets without preferences
// or enterprise policies set.
net::SSLContextConfig GetNewSSLConfig();

class SSLConfigServiceMojo final : public net::SSLConfigService {
 public:
  explicit SSLConfigServiceMojo(net::SSLContextConfig initial_config);

  SSLConfigServiceMojo(const SSLConfigServiceMojo&) = delete;
  SSLConfigServiceMojo& operator=(const SSLConfigServiceMojo&) = delete;

  ~SSLConfigServiceMojo() override;

  // net::SSLConfigService:
  net::SSLContextConfig GetSSLContextConfig() override;
  net::EchMode GetEchMode(std::string_view hostname) const override;
  bool CanShareConnectionWithClientCerts(
      std::string_view hostname) const override;

 private:
  const net::SSLContextConfig ssl_context_config_;
};

}  // namespace mitto

#endif  // MITTO_SSL_CONFIG_SERVICE_MOJO_H_
