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

#include "base/time/time.h"
#include "net/http/http_network_session.h"
#include "net/http/http_server_properties.h"
#include "net/quic/quic_context.h"
#include "net/spdy/spdy_session.h"
#include "net/third_party/quiche/src/quiche/http2/core/spdy_protocol.h"
#include "net/third_party/quiche/src/quiche/quic/core/quic_constants.h"
#include "net/third_party/quiche/src/quiche/quic/core/quic_versions.h"

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

  // Nor does Chrome set this one, so Chrome takes the default in
  // net/http/http_network_session.h: true on Windows, Android and iOS, false
  // elsewhere. The Personas are Windows Chrome, so it is stated, and what a
  // connection does when the network changes does not follow the Host. With
  // true, the sessions go away on an IP address change, so the requests on them
  // can finish. With false they close, and those requests fail with
  // ERR_NETWORK_CHANGED. It does not show on the wire, and it needs the
  // NetworkChangeNotifier that mitto_core.cc creates.
  params->spdy_go_away_on_ip_change = true;
}

void ConfigureQuicParams(net::HttpNetworkSessionParams* params,
                         net::QuicParams* quic_params) {
  // Chrome's ConfigureQuicParams() sets all of these from parameters of a field
  // trial named QUIC. The Chrome that recorded the Baselines had no such trial,
  // as chrome.txt beside them shows, so it took what each one comes to without
  // it. That is also how Chrome reaches QUICHE's flags, through the parameter
  // set_quic_flags, so those stay at their defaults here too. The comments name
  // the parameters, and the order is upstream's. Where a parameter falls back
  // on a feature, chrome.txt shows no study that sets it.

  // enable_quic.
  params->enable_quic = true;

  // retry_without_alt_svc_on_quic_errors.
  quic_params->retry_without_alt_svc_on_quic_errors = true;

  // connection_options. The ORIG in tests/baselines/157/http3.json does not
  // come from here. QuicSessionPool adds it, as feature_list.cc says.
  quic_params->connection_options = {};

  // client_connection_options.
  quic_params->client_connection_options = {};

  // close_sessions_on_ip_change.
  quic_params->close_sessions_on_ip_change = false;

  // goaway_sessions_on_ip_change.
  quic_params->goaway_sessions_on_ip_change = false;

  // idle_connection_timeout_seconds. On the wire this is the transport
  // parameter max_idle_timeout.
  quic_params->idle_connection_timeout = base::Seconds(30);

  // reduced_ping_timeout_seconds.
  quic_params->reduced_ping_timeout = base::Seconds(quic::kPingTimeoutSecs);

  // max_time_before_crypto_handshake_seconds.
  quic_params->max_time_before_crypto_handshake =
      base::Seconds(quic::kMaxTimeForCryptoHandshakeSecs);

  // max_idle_time_before_crypto_handshake_seconds.
  quic_params->max_idle_time_before_crypto_handshake =
      base::Seconds(quic::kInitialIdleTimeoutSecs);

  // estimate_initial_rtt.
  quic_params->estimate_initial_rtt = false;

  // migrate_sessions_on_network_change_v2. The feature it falls back on is on
  // only on Android.
  quic_params->migrate_sessions_on_network_change_v2 = false;

  // use_new_alps_codepoint. The feature it falls back on is on by default. On
  // the wire this is the number of the application_settings extension.
  quic_params->use_new_alps_codepoint = true;

  // migrate_sessions_early_v2.
  quic_params->migrate_sessions_early_v2 = false;

  // allow_port_migration.
  quic_params->allow_port_migration = true;

  // retry_on_alternate_network_before_handshake.
  quic_params->retry_on_alternate_network_before_handshake = false;

  // initial_rtt_for_handshake_milliseconds.
  quic_params->initial_rtt_for_handshake = base::TimeDelta();

  // disable_tls_zero_rtt.
  quic_params->disable_tls_zero_rtt = false;

  // disable_gquic_zero_rtt.
  quic_params->disable_gquic_zero_rtt = false;

  // retransmittable_on_wire_timeout_milliseconds.
  quic_params->retransmittable_on_wire_timeout = base::TimeDelta();

  // enable_debugging_sni_in_transport_param.
  quic_params->enable_debugging_sni_in_transport_param = false;

  // migrate_idle_sessions.
  quic_params->migrate_idle_sessions = false;

  // idle_session_migration_period_seconds.
  quic_params->idle_session_migration_period =
      net::kDefaultIdleSessionMigrationPeriod;

  // multi_port_probing_interval.
  quic_params->multi_port_probing_interval = 0;

  // max_time_on_non_default_network_seconds.
  quic_params->max_time_on_non_default_network =
      net::kMaxTimeOnNonDefaultNetwork;

  // max_migrations_to_non_default_network_on_write_error.
  quic_params->max_migrations_to_non_default_network_on_write_error =
      net::kMaxMigrationsToNonDefaultNetworkOnWriteError;

  // max_migrations_to_non_default_network_on_path_degrading.
  quic_params->max_migrations_to_non_default_network_on_path_degrading =
      net::kMaxMigrationsToNonDefaultNetworkOnPathDegrading;

  // host_whitelist.
  params->quic_host_allowlist = {};

  // initial_delay_for_broken_alternative_service_seconds.
  quic_params->initial_delay_for_broken_alternative_service = std::nullopt;

  // exponential_backoff_on_initial_delay.
  quic_params->exponential_backoff_on_initial_delay = std::nullopt;

  // delay_main_job_with_available_spdy_session.
  quic_params->delay_main_job_with_available_spdy_session = false;

  // enable_origin_frame.
  quic_params->enable_origin_frame = true;

  // skip_dns_with_origin_frame.
  quic_params->skip_dns_with_origin_frame = true;

  // ignore_ip_matching_when_finding_existing_sessions. The feature that goes
  // before it is off by default.
  quic_params->ignore_ip_matching_when_finding_existing_sessions = false;

  // allow_server_migration.
  quic_params->allow_server_migration = true;

  // max_packet_length. The feature that goes before it is off by default. On
  // the wire this is the size of the first datagram, which is 1278 in
  // tests/baselines/157/http3.json once its IP and UDP headers are counted.
  quic_params->max_packet_length = quic::kDefaultMaxPacketSize;

  // quic_version. On the wire this is the version of the Initial packets and
  // the transport parameter version_information.
  quic_params->supported_versions = {quic::ParsedQuicVersion::RFCv1()};
}

void ConfigureQuicHints(net::HttpServerProperties* http_server_properties) {
  // The parameter quic_hints of the feature ConfigureQuicHints, which a source
  // build has off and which the field trial IncreaseHttp3Usage turns on with
  // this value, as chrome.txt beside the Baselines shows. A request to one of
  // these tries QUIC from the start. No Baseline can show that, because the
  // capture server is not one of them.
  http_server_properties->SetKnownQuicAlternativeService("google.com", 443,
                                                         443);
  http_server_properties->SetKnownQuicAlternativeService("www.google.com", 443,
                                                         443);
}

}  // namespace mitto
