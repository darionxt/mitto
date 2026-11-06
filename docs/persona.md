# Persona data

A **Persona** is the browser identity mitto presents to servers. Its data says what one **Chrome Release** on one operating system sends by default for each **Request Kind**. It is package data, so changing it needs no native rebuild. Terms in **bold** are defined in [CONTEXT.md](../CONTEXT.md).

There is one Persona for each Chrome Release in the **Supported Window**, all on Windows: `chrome_155_windows`, `chrome_156_windows` and `chrome_157_windows`. A **Session** takes one when it is created, and `mitto.Session()` and `mitto.get()` present the **Engine Release**'s. A request takes its Request Kind as `kind`, which is navigation for `GET` and fetch for other methods by default:

```python
with mitto.Session(persona="chrome_155_windows") as session:
    session.get(url)
    session.get(url, kind="script")
```

## The file

Each Persona is one file, `src/mitto/personas/NAME.json`, which [`src/mitto/_persona.py`](../src/mitto/_persona.py) reads:

| Key | Meaning |
| --- | --- |
| `release` | The Chrome Release |
| `operating_system` | The operating system the Persona claims, independent of the **Host** |
| `accept_language` | The value for `Accept-Language`, which is a Session setting |
| `kinds.KIND.priority` | The request's priority, such as `HIGHEST`, as in the C API's `MITTO_PRIORITY_HIGHEST` |
| `kinds.KIND.priority_incremental` | Whether the response can be used as it arrives |
| `kinds.KIND.referrer` | The value for `Referer`, or `null`. `<origin>` stands for the origin of the requested URL |
| `kinds.KIND.headers` | `name: value` lines in the order Chrome sends them, with the case HTTP/1.1 sends |

Subresource kinds describe one same-origin **Initiator**, the page at the root of the origin. A Session offers no other Initiator yet.

## What comes from where

The file does not hold everything on the wire, because Chrome does not decide everything in one place either. mitto hands each part to `//net` the way Chrome's network service does, so that it lands where Chrome's does.

| On the wire | Comes from |
| --- | --- |
| `Host`, `Connection`, and the pseudo-headers of HTTP/2 and HTTP/3 | `//net` |
| The lines in `headers` | The C API's `headers`. HTTP/2 and HTTP/3 send the names in lower case |
| `Referer` | The C API's `referrer`. `//net` drops a `Referer` among the headers, and sends this one after them |
| `Accept-Encoding` | `//net`, after `Referer`. The native core turns on Brotli and Zstandard as Chrome does, and `//net` advertises them only over HTTPS and to localhost |
| `Accept-Language` | `//net`, after `Accept-Encoding`, from the Session setting |
| The `priority` header, last in HTTP/2 and HTTP/3 and absent in HTTP/1.1 | `//net`, from `priority` and `priority_incremental` |
| The HTTP/2 stream weight and the HTTP/3 `PRIORITY_UPDATE` | `//net`, from the same two values |

So a line for `Referer`, `Accept-Encoding`, `Accept-Language` or `priority` does not belong in `headers`. Giving `Referer` there is an error, and giving the others would put them in the wrong place, or send `priority` over HTTP/1.1 as well.

## What a caller changes

`Session(accept_language=...)` replaces the Persona's `accept_language`, and an empty string sends no `Accept-Language`. For one request, `headers` changes the Persona's lines, by name in any case:

| In `headers` | On the wire |
| --- | --- |
| A name the Persona has | Takes the place of the Persona's line, which keeps the Persona's spelling of the name, as HTTP/1.1 shows |
| Another name | After the Persona's lines, in the order given, before what `//net` adds |
| A name the Persona has, with `None` | The line is gone |
| Another name, with `None` | Nothing changes, so this cannot remove what `//net` adds |
| `Referer` | Replaces or, with `None`, removes the Persona's `referrer`. `//net` still decides where it goes |

A name that `//net` would add, such as `Accept-Encoding`, counts as another name: it follows the Persona's lines, and `//net` then leaves it alone.

## Write one

Write the file from the Baselines of its Chrome Release in [`tests/baselines`](../tests/baselines), by taking away what the table above leaves to `//net`. Over the three protocols, a Request Kind differs only in what `//net` adds and in the case of its names, so one list of lines serves all three.

[`tests/test_persona.py`](../tests/test_persona.py) compares the file with every Baseline of its Chrome Release, including HTTP/3, without a native library. It writes out what `//net` adds. The [Parity Checks](parity.md) then show what `//net` really does.

## What differs between Chrome Releases

An older Chrome Release runs on the network code of the **Engine Release**, and differs only by its Persona ([ADR-0005](adr/0005-older-releases-as-data.md)). Chrome 155 and 156 need nothing else: [their Baselines](baselines.md#what-differs-from-chrome-157) differ from Chrome 157's in `sec-ch-ua` and `User-Agent` only, so those two lines are all that differs between the three files. The Baselines of [the Recording Cases](baselines.md#the-recording-cases) agree, with redirects, bodies, cookies, plain HTTP and a proxy: every case differs in those two lines only. Plain HTTP and a `CONNECT` have no `sec-ch-ua`, so there it is `User-Agent` alone.

[`tools/workspace/compare`](building.md#compare-two-releases), run from each of 155.0.8059.85 and 156.0.8078.80 to 157.0.8103.27, agrees with that. These files are the same in all three, so what sets up a connection did not move:

| Where | Files |
| --- | --- |
| `chromium/src` | `components/network_session_configurator/browser/network_session_configurator.cc`, `services/network/ssl_config_service_mojo.cc`, `services/network/ssl_config_type_converter.cc`, `net/socket/ssl_client_socket_impl.cc`, `net/http/http_network_session.{h,cc}` and `net/quic/quic_context.{h,cc}` |
| BoringSSL | `ssl/handshake_client.cc` and `ssl/ssl_key_share.cc`. `ssl/extensions.cc` only gains a stricter check of trust anchor IDs, and from 155 some renames |
| QUICHE | `tls_client_handshaker.cc`, `crypto/transport_parameters.cc`, `http/quic_spdy_session.cc`, `http/quic_spdy_client_session.cc`, `http/http_encoder.cc`, `quic_packet_creator.cc` and `quic_versions.cc` under `quiche/quic/core` |

The trust anchor IDs in the compiled-in root store are the same 28 in all three, although its version goes from 39 to 40 to 42.

If a later Chrome Release does differ, the difference has to reach the native core per Session, because one process can hold Sessions of several Personas:

- `GetNewSSLConfig()`, `ConfigureHttp2Params()`, `ConfigureQuicParams()` and `ConfigureQuicHints()` in the native core are per Session, so a value there becomes a Session setting that comes from Persona data.
- `native/feature_list.cc` is per process, so its features state one thing for every Persona. A Chrome Release that differs there needs the per-Session value that the feature feeds. For example, a different `google_connection_options` goes in `QuicParams::connection_options` with `TryQuicByDefault` off, or `ORIG` goes out twice, and a Chrome Release without `trust_anchors` needs an empty `SSLContextConfig::trust_anchor_ids` and not another state of `TLSTrustAnchorIDs`.
- A difference that is neither is a decision for the milestone's plan, and does not stay on the known-gaps list.

## What no Baseline covers

The Baselines of the main **Request Kinds** only record HTTPS. Over plain HTTP to a host that is not local, Chrome leaves out `sec-ch-ua*` and `Sec-Fetch-*`, because those URLs are not potentially trustworthy, as `cases/plain_http.json` shows, and the data does not change for that yet. `//net` does make the change for `Accept-Encoding`.

Baselines record a fresh connection that goes well. Everything else is the Engine Release's whichever Persona is presented, which is the imperfect emulation that [ADR-0005 accepts](adr/0005-older-releases-as-data.md#consequences). Between 155 and 157, it changed in these places:

| What | Where it shows |
| --- | --- |
| The HSTS preload list. Chrome 155 and 156 have the same one. 2,730 hosts are only in Chrome 157's and 2,361 only in theirs | A plain HTTP request to one of those hosts: the native core asks for HTTPS as Chrome 157 does, whichever Persona it presents |
| QUIC connection migration, in `net/quic/quic_chromium_client_session.cc` and `quic_migration_attempt_context.cc`, with new features and new defaults | When the network or the path changes |
| Closing an HTTP/2 session after a protocol error, in `net/spdy/spdy_session.cc` | When a server breaks the protocol |
| `HttpCacheZstdCompression` and `HttpCacheZstdDecompression` by default from 157, and `LogicalClearHttpCache` from 156 | The HTTP cache, which a Session does not have yet, and never on the wire |
