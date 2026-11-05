// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file of Chromium's source tree.
//
// Ported from components/network_session_configurator/browser/
// network_session_configurator.h, whose build target brings in
// //components/variations. A source build sees no field trials, so instead of
// reading them this states the values that Chrome ends up with. Reading the
// command line is left out.

#ifndef MITTO_NETWORK_SESSION_CONFIGURATOR_H_
#define MITTO_NETWORK_SESSION_CONFIGURATOR_H_

namespace net {
class HttpServerProperties;
struct HttpNetworkSessionParams;
struct QuicParams;
}  // namespace net

namespace mitto {

// Sets the HTTP/2 connection settings in `params` to the values Chrome uses.
void ConfigureHttp2Params(net::HttpNetworkSessionParams* params);

// Sets the QUIC settings in `params` and `quic_params` to the values Chrome
// uses. That leaves `quic_params->origins_to_force_quic_on` and
// `quic_params->force_quic_everywhere` alone, since in Chrome they only come
// from the command line.
void ConfigureQuicParams(net::HttpNetworkSessionParams* params,
                         net::QuicParams* quic_params);

// Tells `http_server_properties` which origins Chrome knows to speak QUIC
// before it has seen an Alt-Svc from them. Chrome does this while constructing
// HttpServerProperties, in MaybeProcessQuicHints(). That reads a feature, and a
// feature cannot vary per Session, so this is a call of its own.
void ConfigureQuicHints(net::HttpServerProperties* http_server_properties);

}  // namespace mitto

#endif  // MITTO_NETWORK_SESSION_CONFIGURATOR_H_
