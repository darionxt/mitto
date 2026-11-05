// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file of Chromium's source tree.
//
// Ported from components/network_session_configurator/browser/
// network_session_configurator.h, whose build target brings in
// //components/variations. A source build sees no field trials, so instead of
// reading them this states the values that Chrome ends up with. Reading the
// command line is left out, and so is QUIC for now.

#ifndef MITTO_NETWORK_SESSION_CONFIGURATOR_H_
#define MITTO_NETWORK_SESSION_CONFIGURATOR_H_

namespace net {
struct HttpNetworkSessionParams;
}  // namespace net

namespace mitto {

// Sets the HTTP/2 connection settings in `params` to the values Chrome uses.
void ConfigureHttp2Params(net::HttpNetworkSessionParams* params);

}  // namespace mitto

#endif  // MITTO_NETWORK_SESSION_CONFIGURATOR_H_
