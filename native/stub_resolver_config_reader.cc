// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file of Chromium's source tree.
//
// Ported from NetworkService::ConfigureStubHostResolver() in
// services/network/network_service.cc, with the values that
// chrome/browser/net/stub_resolver_config_reader.cc passes it by default.

#include "mitto/stub_resolver_config_reader.h"

#include <utility>
#include <vector>

// //net/dns keeps this header to the targets on its friend lists, to track who
// uses it outside the network stack. //mitto uses it as //services/network
// does.
#include "net/dns/host_resolver_manager.h"  // nogncheck
#include "net/dns/public/dns_config_overrides.h"
#include "net/dns/public/dns_over_https_config.h"
#include "net/dns/public/insecure_dns_mode.h"
#include "net/dns/public/secure_dns_mode.h"

namespace mitto {

void ConfigureStubHostResolver(
    net::HostResolverManager& host_resolver_manager,
    const std::optional<net::IPEndPoint>& dns_nameserver) {
  // prefs::kBuiltInDnsClientEnabled defaults to ShouldEnableAsyncDns(), which
  // is true because kAsyncDns is on by default on Windows, macOS and Linux. The
  // one exception is a Windows machine with Zero Trust DNS turned on.
  // kChromeEnableDnsPlatform is off by default, so this is the built-in client.
  // prefs::kAdditionalDnsQueryTypesEnabled defaults to true, which is what asks
  // for HTTPS records.
  host_resolver_manager.SetInsecureDnsClientEnabled(
      net::InsecureDnsMode::kEnabledBuiltIn,
      /*additional_dns_types_enabled=*/true);

  net::DnsConfigOverrides overrides;
  if (dns_nameserver) {
    overrides =
        net::DnsConfigOverrides::CreateOverridingEverythingWithDefaults();
    overrides.nameservers = std::vector<net::IPEndPoint>{*dns_nameserver};
  }
  // prefs::kDnsOverHttpsTemplates defaults to empty.
  overrides.dns_over_https_config = net::DnsOverHttpsConfig();
  // DefaultDnsOverHttpsConfigSource makes this the default of
  // prefs::kDnsOverHttpsMode.
  overrides.secure_dns_mode = net::SecureDnsMode::kAutomatic;
  // features::kDnsOverHttpsUpgrade, which is on by default on Windows, macOS
  // and Linux. It lives outside //net, so the native core has no such feature.
  overrides.allow_dns_over_https_upgrade = true;
  // ShouldUseDohFallback() needs a preference that defaults to false.
  overrides.fallback_doh_nameservers = std::vector<net::IPEndPoint>();
  host_resolver_manager.SetDnsConfigOverrides(std::move(overrides));
}

}  // namespace mitto
