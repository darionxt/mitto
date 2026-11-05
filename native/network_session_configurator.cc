// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file of Chromium's source tree.
//
// Ported from components/network_session_configurator/browser/
// network_session_configurator.cc. See the header for what is left out.

#include "mitto/network_session_configurator.h"

#include <stddef.h>
#include <stdint.h>

#include <optional>

#include "net/http/http_network_session.h"
#include "net/spdy/spdy_session.h"
#include "net/third_party/quiche/src/quiche/http2/core/spdy_protocol.h"

namespace mitto {
namespace {

// These four have the same names and values as the defaults in
// net/http/http_network_session.cc and .h. They are repeated so that a new
// default in //net cannot change what mitto sends without a Parity Check
// failing. On the wire they are what tests/baselines/157/http2.json holds under
// http2.preface.

// How much a connection may receive. The WINDOW_UPDATE on stream 0 raises the
// window to this from the protocol's initial 65535, an increment of 15663105.
constexpr size_t kSpdySessionMaxRecvWindowSize = 15 * 1024 * 1024;

// How much a stream may receive, sent as SETTINGS_INITIAL_WINDOW_SIZE.
constexpr uint32_t kSpdyStreamMaxRecvWindowSize = 6 * 1024 * 1024;

// The largest HPACK dynamic table the server may use for what it sends, sent as
// SETTINGS_HEADER_TABLE_SIZE. Nothing else about HPACK can be set: //net always
// compresses, and its encoder's table follows the server's SETTINGS.
constexpr uint32_t kSpdyMaxHeaderTableSize = 64 * 1024;

// The largest header list the server may send, sent as
// SETTINGS_MAX_HEADER_LIST_SIZE.
constexpr uint32_t kSpdyMaxHeaderListSize = 256 * 1024;

}  // namespace

void ConfigureHttp2Params(net::HttpNetworkSessionParams* params) {
  // Chrome's ConfigureHttp2Params() sets the next six from parameters of a
  // field trial named HTTP2, or from command line switches. The Chrome that
  // recorded the Baselines had neither, as chrome.txt beside them shows, so it
  // took what each one comes to without them. The comments name the parameters.

  // http2_enabled.
  params->enable_http2 = true;

  // http2_settings, which is empty in Chrome, so that it gets the defaults
  // above from HttpNetworkSession. SETTINGS_ENABLE_PUSH is not here because
  // HttpNetworkSession sends 0 for it whatever this holds.
  params->http2_settings = {
      {spdy::SETTINGS_HEADER_TABLE_SIZE, kSpdyMaxHeaderTableSize},
      {spdy::SETTINGS_INITIAL_WINDOW_SIZE, kSpdyStreamMaxRecvWindowSize},
      {spdy::SETTINGS_MAX_HEADER_LIST_SIZE, kSpdyMaxHeaderListSize},
  };

  // http2_grease_settings.
  params->enable_http2_settings_grease = false;

  // http2_grease_frame_type.
  params->greased_http2_frame = std::nullopt;

  // http2_end_stream_with_data_frame.
  params->http2_end_stream_with_data_frame = false;

  // spdy_session_max_queued_capped_frames. Without the parameter Chrome uses
  // this same constant. It does not show on the wire, so no Parity Check would
  // notice if a copy of its value here went out of date.
  params->spdy_session_max_queued_capped_frames =
      net::kSpdySessionMaxQueuedCappedFrames;

  // Chrome does not set this one anywhere, and no field trial reaches it.
  params->spdy_session_max_recv_window_size = kSpdySessionMaxRecvWindowSize;
}

}  // namespace mitto
