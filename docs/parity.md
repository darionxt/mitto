# Parity Checks

A **Parity Check** compares what the public API sends through the native core with what real Chrome sent, as recorded in a **Baseline**. Everything the rest of milestone M1 does is measured with them. Terms in **bold** are defined in [CONTEXT.md](../CONTEXT.md).

## Run them

The checks need a native library ([building.md](building.md)) and no network connection, because they only talk to a capture server on loopback:

```bash
export MITTO_NATIVE_LIBRARY="$MITTO_WORKSPACE/chromium-<tag>/src/out/mitto/libmitto_core.dylib"
uv run pytest tests/parity
```

On Linux the file is `libmitto_core.so`. Without a native library, the tests that make requests are skipped and the others still run.

## What a check does

For each Baseline, it:

1. starts a [capture server](capture.md);
2. creates a new `mitto.Session` with the **Persona** of the Baseline's **Chrome Release**. It accepts the server's private certificate, through a setting that only the private `_settings` argument reaches. For an `h3` Baseline it also forces QUIC on the server's HTTP/3 origin, as the Chrome that recorded it was told to, because `//net` refuses QUIC to a private certificate otherwise;
3. requests the path of the Baseline's **Request Kind** on the port of the Baseline's protocol, with that kind as `kind`, so with what the Persona sends for it ([persona.md](persona.md));
4. takes the normal form of what the server received, and compares it with the Baseline.

Each comparison covers what the connection looks like and the one request. So a difference in the connection, such as in the ClientHello, shows up once for each Request Kind.

The native core of the **Engine Release** presents every Chrome Release in turn, so one native library checks all of them. A Baseline of a Chrome Release that has no Persona stops the run.

## Baselines

A Baseline file is the normal form the capture kit writes, at `tests/baselines/RELEASE/NAME.json`, where `RELEASE` is the **Chrome Release**. The file holds one Baseline for each Request Kind in it, and the protocol comes from the file's `protocol` key. [baselines.md](baselines.md) is how to record them.

Other files under `RELEASE` are left alone: `*.raw.json` and anything that does not end in `.json`, such as `chrome.txt`, which says which Chrome recorded the Baselines. So is `RELEASE/cases`, which holds the **Recording Cases** beyond the main **Request Kinds**. No check compares those yet.

## Gaps

A gap is one field in which the two normal forms differ. A failure names the Chrome Release, Request Kind, protocol and field, with both values:

```text
Chrome 157, navigation, h2, request.headers.accept-encoding
  Baseline: "accept-encoding: gzip, deflate, br, zstd"
  mitto:    "accept-encoding: gzip, deflate"
```

`(nothing)` means that side does not have the field. The comparison looks only at the shape of the data, so new fields in the normal form need no change here:

| Data | Compares | Example field |
|---|---|---|
| An object | Key by key | `tls.legacy_version` |
| A list of objects with a `type` | Item by item, by `type` | `tls.extensions.key_share.shares` |
| A list of `name: value` lines | Line by line, by name, with the case sent | `request.headers.user-agent`, `http2.preface.SETTINGS.settings.ENABLE_PUSH` |
| Anything else, such as a list of plain values | As a whole | `tls.cipher_suites` |

The request's own fields come under `request`. If several items have the same `type` or name, the later ones get a number, such as `tls.extensions.GREASE#2`. If the items both sides have are in a different order, that is one more gap, in a field such as `request.headers.<order>`, whose values are the names in each order.

A request that gets no response has nothing to compare. It is one gap in the field `response`, whose value for mitto is the net error.

## The known-gaps list

`tests/baselines/known_gaps.json` holds every **Known Gap**, one per line, with the keys `release`, `kind`, `protocol`, `field`, `baseline` and `mitto`. Here `null` means that side does not have the field.

A run fails on a gap that is not in the list and on a gap in the list that no longer exists. A gap whose value has changed counts as both, so the list always says exactly how mitto differs from the Baselines.

There is one list for both **Hosts**, because a Wire Fingerprint must not depend on the Host. A gap on only one Host fails the run on the other.

To write the list again from what the native core does now:

```bash
uv run python -m tools.parity
```

This needs a native library, and it runs the same comparison as the tests. `--baselines DIR` uses another directory of Baselines, with its own list. Read the diff before you commit it. Fixing a gap only removes lines. An added line means mitto moved away from a Baseline, or a Baseline changed.

## What the native core states

Real Chrome gets its settings from preferences, field trials and the defaults in `//net`, and a source build has only the defaults ([ADR-0001](adr/0001-chromium-network-stack.md)). So the native core sets them itself, in code ported from Chrome with its names kept:

| File | States | Upstream |
|---|---|---|
| `native/network_session_configurator.cc` | HTTP/2 connection settings, and with `enable_http2` the ALPN list. QUIC parameters, and the origins that a field trial tells Chrome speak QUIC | `components/network_session_configurator`, and the hints in `net/http/http_server_properties.cc` |
| `native/ssl_config_service_mojo.cc` | TLS versions, cipher suites, key shares, ECH GREASE and trust anchors | `chrome/browser/ssl/ssl_config_service_manager.cc`, `services/network/ssl_config_service_mojo.cc` and `ssl_config_type_converter.cc` |
| `native/feature_list.cc` | Features that the ClientHello depends on, over TCP and QUIC, and those in `//net` that field trials turn on | The `--enable-features` and `--disable-features` lists in `chrome.txt` |
| `native/stub_resolver_config_reader.cc` | The stub resolver: the built-in DNS client, additional query types, which is what asks for HTTPS records, secure mode automatic, and the upgrade to DNS over HTTPS | `ConfigureStubHostResolver` in `services/network/network_service.cc`, with the defaults in `chrome/browser/net/stub_resolver_config_reader.cc` |

A value stated here stays when a new **Engine Release** changes a default, so a Parity Check fails instead of mitto moving with the engine. That matters because an older **Chrome Release** must keep sending what it sent. All of this holds for every Persona, since Chrome 155, 156 and 157 [send the same in it](persona.md#what-differs-between-chrome-releases).

When new Baselines list a feature in `chrome.txt` that the native core's build holds, add it to `native/feature_list.cc`. A feature outside that build, such as one in `//content`, does nothing in the native core. `chrome.txt` holds only names that look network-related, and a recording draws one group of each study, so a study with a small group may not show. The variations seed itself holds every study.

The trust anchors come from the root store compiled into `//net`, as they do in a Chrome that has just started. mitto has no component updates, so the list stays that of the Engine Release until the next one, whatever a Chrome that has run for a while has moved to ([what can differ](capture.md#what-can-differ-between-chrome-installations)).

`native/mitto_core.cc` creates one `NetworkChangeNotifier` and one `HostResolverManager` per process, as the network service does, and each **Session** has its own cache of answers on that manager. A Session starts DoH probes 5 seconds after the process created them, the default of `kDelayInitialDohProbeTimeoutParam`.

The first Session in a process waits up to a second for the system's DNS settings, which Chrome does not do, because Chrome has them long before a page asks for a host. A program's first request can come right away, and without those settings it would go to the system's resolver, which sees no HTTPS records.

No Parity Check can see any of this. The capture server's names are never looked up, because a `.localhost` name resolves to loopback by itself and a `.test` name goes through `--host-resolver-rules`, so a Baseline's ClientHello always holds ECH GREASE. `tests/test_dns.py` checks real ECH and an `h3` HTTPS record against a DNS server on loopback, which a Session reaches through the `dns_nameserver` setting. Only `mitto._native` offers that setting, and the public Session takes it only through `_settings`. Such a Session has a `HostResolverManager` of its own, so that other Sessions keep the system's settings.

## Tests of the comparison

Real Baselines are recorded by hand on a Windows machine, so `tests/parity` tests the comparison itself with a sample: the real normal forms of Chrome 156 on macOS in `tests/capture/data`. It changes that sample on purpose and expects the run to fail, both through the native core and, where there is no native library, through a stand-in for it.
