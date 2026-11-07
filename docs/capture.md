# The capture kit

The capture kit records what a client sends to a server: the raw TLS ClientHello, the Initial packets of QUIC, the decrypted HTTP/3 and HTTP/2 frames or HTTP/1.1 requests, a `CONNECT` to its proxy, and a **normal form** of them that compares equal between recordings of the same client. The normal form is also the **Baseline** file format, and **Parity Checks** compare in it. Terms in **bold** are defined in [CONTEXT.md](../CONTEXT.md).

The kit lives in `tools/capture`. It needs nothing but [uv](https://docs.astral.sh/uv/) and runs on Windows, macOS and Linux. It does not need the native core, and its dependencies are in the development-only `dev` group.

## Record a client

From the repository root, start the server:

```bash
uv run python -m tools.capture --out recordings
```

It prints the hash of a certificate it just generated, and the address and Chrome flags of each **run**, which is one start of a Chrome and the file it fills:

```text
SPKI hash: s7oEk1pfWBlrJ4HC8N6EMoT0vFzSxCKHUgHGBCo1Ejc=

Start each recording from a new, empty user data directory:
  chrome --user-data-dir="NEW_EMPTY_DIRECTORY" --no-first-run --no-default-browser-check FLAGS URL

HTTP/1.1: https://capture.localhost:8443/
  FLAGS: --ignore-certificate-errors-spki-list=s7oEk1pfWBlrJ4HC8N6EMoT0vFzSxCKHUgHGBCo1Ejc=
HTTP/3: https://capture.localhost:8445/
  FLAGS: --ignore-certificate-errors-spki-list=s7oEk1pfWBlrJ4HC8N6EMoT0vFzSxCKHUgHGBCo1Ejc= --origin-to-force-quic-on=capture.localhost:8445
Cases over HTTP/2: https://capture.test:8444/case/start
  FLAGS: --ignore-certificate-errors-spki-list=s7oEk1pfWBlrJ4HC8N6EMoT0vFzSxCKHUgHGBCo1Ejc= --host-resolver-rules="MAP capture.test 127.0.0.1,MAP same-site.capture.test 127.0.0.1,MAP cross-site.test 127.0.0.1" --no-proxy-server
...
```

Open each address in a Chrome started that way. Chrome ignores `--ignore-certificate-errors-spki-list` unless `--user-data-dir` is there too. Over QUIC it needs both that flag and `--origin-to-force-quic-on`, or it refuses a certificate from an unknown root. Each address loads a page, which tells the server when it has finished, and a case page asks you to click on the way. The server then writes that run's files and, once it has them all, exits:

| File | Holds |
|---|---|
| `recordings/http1.json`, `recordings/http2.json`, `recordings/http3.json` | The normal form of the main **Request Kinds** over each protocol |
| `recordings/cases/*.json` | The normal form of the other **Recording Cases** |
| `recordings/*.raw.json`, `recordings/cases/*.raw.json` | The raw recording of each: every connection that came in by the same way, with the client's address, its ClientHello bytes, its Initial packets over QUIC, every frame and every request |
| `recordings/runs.json` | The name, address and flags of each run, for a script |

To check that a client records the same twice, record it into two directories and compare the files.

`--host` sets another name ending in `.localhost`. `--http1-port`, `--http2-port`, `--http3-port`, `--plain-http-port` and `--proxy-port` set other ports. The normal form does not depend on any of them. `--runs` names the runs to wait for, such as `--runs http1 http2 http3`.

`--click-in USER_DATA_DIRECTORY` does the clicking, in a Chrome with that directory and `--remote-debugging-port=0`. A navigation counts as the user's only with a user gesture, and Chrome sends `Sec-Fetch-User` only then, so a page cannot click for itself. This runs the page's own click on the page's own element through `Runtime.evaluate` with `userGesture`, which goes around the window's input. In headless Chrome 156 on macOS, real mouse events through `Input.dispatchMouseEvent` gave identical normal forms over all three protocols.

## What the server does

- It serves at a `*.localhost` name. Chrome and the network stack resolve these names to loopback themselves, with no DNS or hosts file entry, and unlike an IP address such a name gets SNI. The server listens only on loopback.
- It also answers under three names that are [not local](#recording-cases), which a client has to be told to send to loopback. Its one certificate names them all, so a client may pool their connections over HTTP/2 and HTTP/3.
- It serves HTTP/1.1 and HTTP/2 on separate TCP ports, and HTTP/3 on a UDP port. Each port offers one protocol through ALPN. On a TCP port, a client that does not take it is served HTTP/1.1, and the normal form shows that. Over QUIC, such a client gets no connection.
- It serves plain HTTP/1.1 on another TCP port.
- It is an HTTP proxy on another TCP port. It answers `CONNECT` to one of its names on its HTTP/1.1 or HTTP/2 port by serving that port's protocol in the tunnel, on the same socket, so a tunnel reaches nothing outside the server. It refuses anything else with a 403. What comes through it gets its own recording, whose connections also hold the `CONNECT` that opened them.
- It gives out no session tickets. Every connection is therefore a fresh one, with no `pre_shared_key`, including the later connections of one page load.
- Its private certificate is new on every start, so the hash changes too.
- Its HTTP/2 side does only what the pages need. Responses use no HPACK dynamic table and ignore flow control, because they are far smaller than the windows Chrome opens. A client whose window is smaller than a response is not served correctly. It sends no `WINDOW_UPDATE`, so a client can send it no more than the 64 KiB of the default window.
- It waits for the end of a request's body before it answers. Over HTTP/1.1 it keeps a body of up to 16 MiB that comes with `Content-Length`. A chunked body has no length to wait for and is not recorded, so a request that has one gets an answer at once and the connection ends with it.
- [aioquic](https://github.com/aiortc/aioquic) is its QUIC and TLS stack, and [pylsqpack](https://github.com/aiortc/pylsqpack) its QPACK. Everything recorded comes from the kit's own parsers, which read Initial packets before aioquic does and HTTP/3 streams after it.
- It speaks QUIC v1 only and answers a client of another version with Version Negotiation. It never sends Retry, so the first Initial packets hold no token.
- Its HTTP/3 side allows a client's QPACK encoder a dynamic table of 4096 bytes and 16 blocked streams, and its responses use no table. A client's QPACK decoder therefore has nothing to say, so a client that opens its decoder stream lazily, as Chrome does, shows no such stream. A frame over 1 MiB ends the connection.

Each main **Request Kind** has a path, and the test page requests all of them from one same-origin **Initiator**:

| Request Kind | Path | Requested by |
|---|---|---|
| navigation | `/` | Opening the address |
| script | `/script.js` | A `<script>` element in the page |
| image | `/image.png` | An `<img>` element in the page |
| fetch | `/fetch` | `fetch()` in the page |

When those have finished, the page requests `/done`, which is no Request Kind. A request to any other path gets a 404, and it stays in the raw recording only.

## Recording Cases

Each main Request Kind makes one **Recording Case**, named after it. The other cases have their own paths, under `/case/`, and pages that make a real Chrome produce them. Those pages are at three names, which `--host-resolver-rules` sends to loopback:

| Name | Is |
|---|---|
| `capture.test` | The page, the **Initiator** |
| `same-site.capture.test` | Another origin of the same site |
| `cross-site.test` | Another site |

`.test` is reserved, so these names are nobody's. Unlike a `*.localhost` name, they are not local. So a proxy applies to them, plain HTTP to them is not potentially trustworthy, and Chrome treats them as it does a host on a network.

| Run | File | Way in | Recording Cases |
|---|---|---|---|
| HTTP/1.1, HTTP/2, HTTP/3 | `http1.json`, `http2.json`, `http3.json` | The port of that protocol | `navigation`, `fetch`, `script`, `image` |
| Cases over HTTP/1.1, HTTP/2, HTTP/3 | `cases/http1.json`, `cases/http2.json`, `cases/http3.json` | The port of that protocol | The table below |
| Cases over plain HTTP | `cases/plain_http.json` | The plain HTTP port | `plain_http_navigation`, `plain_http_fetch`, `plain_http_script`, `plain_http_image` |
| Cases through a proxy, over HTTP/1.1 and HTTP/2 | `cases/proxy_http1.json`, `cases/proxy_http2.json` | The proxy, to the port of that protocol | `proxy_connect`, `proxy_navigation` |
| Client hints over HTTP/1.1, HTTP/2, HTTP/3 | `cases/client_hints_http1.json`, `cases/client_hints_http2.json`, `cases/client_hints_http3.json` | The port of that protocol | [Those below](#client-hints) |
| Client hints over plain HTTP | `cases/client_hints_plain_http.json` | The plain HTTP port | `client_hints_plain_http_fetch`, `client_hints_plain_http_navigation` |

The three runs over TLS go through the same pages, in this order, and the page at each step shows what to click:

| Recording Case | What makes it |
|---|---|
| `script_same_site`, `script_cross_site`, `image_same_site`, `image_cross_site` | `<script>` and `<img>` elements in the first page |
| `fetch_get_same_site`, `fetch_get_cross_site` | `fetch()` |
| `fetch_post_text_same_origin`, `fetch_post_text_cross_site` | `fetch()` with a string, which gives it `text/plain` and no preflight |
| `fetch_post_json_same_origin` | `fetch()` with `Content-Type: application/json` |
| `fetch_post_stream` | `fetch()` with a `ReadableStream` |
| `cookie` | `fetch()` after another path set a cookie. Its `Path` keeps it off every other request |
| `revalidation` | The second `fetch()` of a path whose response has `ETag` and `Cache-Control: no-cache`. It gets a 304 |
| `redirect_fetch_1` to `redirect_fetch_4` | One `fetch()`, whose hops go same-origin, same-site, cross-site and back to the page's origin |
| `navigation_link_same_origin`, `navigation_link_same_site`, `navigation_link_cross_site` | A click on a link in each page in turn |
| `navigation_form_urlencoded`, `navigation_form_multipart` | A click on the button of a form, with names and values that take escaping. The second has a file |
| `redirect_navigation_1` to `redirect_navigation_4` | A click on a link, with the same hops as the fetch |

Every response under `/case/` allows every origin, because a request shows no sign of that and a page cannot go on without it. So the cases need no preflight, and each hop of a redirect also works with `Origin: null`.

### Client hints

What a server asks for lasts as long as the user data directory, so these runs come last. Their files stay apart from the others, which are the same with or without these. The three runs over TLS go through these pages in order, starting at `client_hints_first`, which asks for every hint in `Accept-CH`:

| Recording Case | What makes it |
|---|---|
| `client_hints_first` | The navigation to the page that asks, which has no hint yet |
| `client_hints_script`, `client_hints_image`, `client_hints_fetch` and each with `_same_site` and `_cross_site` | The elements and `fetch()` of that page. What is at another origin also says `Accept-CH`, which counts for nothing because it is no navigation |
| `client_hints_fetch_post` | `fetch()` with a string. So this and `client_hints_form` hold `Content-Type` among the hints |
| `client_hints_redirect_fetch_1` to `_4` | One `fetch()`, whose hops go same-origin, same-site, cross-site and back |
| `client_hints_navigation`, `client_hints_navigation_same_site`, `client_hints_navigation_cross_site`, `client_hints_navigation_back` | A click on a link in each page in turn, which ends where it began |
| `client_hints_form` | A click on the button of a form |
| `client_hints_redirect_navigation_1` to `_5` | A click on a link, whose hops go to the page's origin, to the same site twice, where the first of those answers `Accept-CH` itself, to another site and back |
| `client_hints_one_*`, `client_hints_few_*`, `client_hints_many_*`, `client_hints_legacy_*`, `client_hints_none_*` | From a page that asks for one hint, three, nine, the six without `sec-ch-`, or with an empty header for nothing: a `fetch()` as `_fetch`, one with a string as `_post`, and a click on a link as `_navigation` |
| `client_hints_critical` | A click on a link to a page at another site, which says `Accept-CH` and `Critical-CH`. Chrome asks again with the hint, and the case is the request that has it |

The images come from a script. Chrome's preload scanner finds an image in the HTML, and raises its priority unless it says it is small. When it says so, Chrome sends its width in `width` and `sec-ch-width`, which depend on the page's layout. An image from a script has neither, and has the priority of `image`. The set of nine is what real servers often ask for, and how many there are decides [the order of a subresource's lines](parity.md#the-order-of-a-subresources-lines), which is why there are several.

The run over plain HTTP asks for every hint too, and then makes a `fetch()` and a navigation that shows that it did not count.

### A stream body over HTTP/1.1

Chrome never sends it. It settles on HTTP/1.1 and then fails the request with `ERR_ALPN_NEGOTIATION_FAILED`, so no bytes of it reach the server, and in the page the `fetch()` rejects with `TypeError: Failed to fetch`. The page then asks for `/case/fetch-post-stream-refused`, and in that recording the normal form has `{"refused": true}` as the case's entry.

## The normal form

A normal form is JSON. Its keys are always in the same order, and a Baseline file is exactly this text.

An HTTP/2 connection looks like this:

```json
{
  "format": 1,
  "protocol": "h2",
  "tls": {
    "record_version": "TLS 1.0",
    "legacy_version": "TLS 1.2",
    "session_id_length": 32,
    "cipher_suites": ["GREASE", "TLS_AES_128_GCM_SHA256", "..."],
    "compression_methods": ["null"],
    "extensions": [
      {"type": "GREASE", "length": 0},
      {"type": "supported_groups", "groups": ["GREASE", "X25519MLKEM768", "x25519", "..."]},
      {"type": "encrypted_client_hello", "client_hello": "outer", "kdf": "HKDF-SHA256", "aead": "AES-128-GCM"}
    ]
  },
  "http2": {
    "preface": [
      {"type": "SETTINGS", "settings": ["HEADER_TABLE_SIZE: 65536", "ENABLE_PUSH: 0", "..."]},
      {"type": "WINDOW_UPDATE", "stream": 0, "increment": 15663105}
    ]
  },
  "requests": {
    "navigation": {
      "flags": ["END_STREAM", "END_HEADERS", "PRIORITY"],
      "priority": {"exclusive": true, "weight": 256},
      "headers": [":method: GET", ":authority: <authority>", ":scheme: https", ":path: /", "..."]
    }
  }
}
```

And an HTTP/3 connection like this:

```json
{
  "format": 1,
  "protocol": "h3",
  "quic": {
    "version": "QUIC v1",
    "destination_connection_id_length": 8,
    "source_connection_id_length": 0,
    "token_length": 0,
    "first_packet_number": 1,
    "first_packet_number_length": 1,
    "first_datagram_ip_size": 1278
  },
  "tls": {
    "legacy_version": "TLS 1.2",
    "session_id_length": 0,
    "cipher_suites": ["TLS_AES_128_GCM_SHA256", "..."],
    "compression_methods": ["null"],
    "extensions": [
      {"type": "supported_groups", "groups": ["X25519MLKEM768", "x25519", "..."]},
      {"type": "quic_transport_parameters", "parameters": [
        {"type": "GREASE"},
        {"type": "max_idle_timeout", "value": 30000},
        {"type": "initial_source_connection_id", "length": 0},
        {"type": "version_information", "chosen": "QUIC v1", "others": ["GREASE", "QUIC v1"]},
        {"type": "google_connection_options", "options": ["ORIG"]}
      ]}
    ]
  },
  "http3": {
    "streams": [{"type": "control", "stream": 2}, {"type": "qpack_encoder", "stream": 10}],
    "control": [
      {"type": "SETTINGS", "settings": ["QPACK_MAX_TABLE_CAPACITY: 65536", "...", "GREASE"]},
      {"type": "GREASE"}
    ]
  },
  "requests": {
    "navigation": {
      "headers": [":method: GET", ":authority: <authority>", ":scheme: https", ":path: /", "..."],
      "priority_updates": ["u=0, i"]
    }
  }
}
```

(The server writes one array item per line. These examples are shortened.)

| Key | Meaning |
|---|---|
| `format` | The version of this format. |
| `protocol` | The protocol the connection settled on: `http/1.1`, `h2` or `h3`. |
| `quic` | Only for `h3`. What the first Initial packet says about the connection, and the first datagram's size counting its IP and UDP headers. |
| `tls` | The ClientHello. Over QUIC it comes without TLS records, so it has no `record_version`. |
| `http2` | Only for `h2`. `preface` holds the frames the client sent before its first request, without acknowledgements of what the server sent. |
| `http3` | Only for `h3`. `streams` lists the unidirectional streams the client opened, and `control` the frames on its control stream, without those listed below. |
| `requests` | One entry per Recording Case recorded, always in the order navigation, fetch, script, image and then those of [the tables above](#recording-cases). If a case was requested twice, the first request counts. `cookie` and `revalidation` only count a request with `Cookie` or `If-None-Match`, so the first request with one counts. |

An HTTP/1.1 request has `request_line` and `headers`. An HTTP/2 request has the `flags` of its HEADERS frame, the frame's `priority` if it has any, `headers` and, if there were any, `later_frames`: what the client sent on the stream after the headers, without the content of bodies or encoded header blocks. An HTTP/3 request has `headers` and, if there were any, `priority_updates`: the priorities that PRIORITY_UPDATE frames on the control stream gave it, in order. It also has `earlier_frames` and `later_frames`: what the client sent on the stream before and after the first headers, without the content of bodies or encoded header sections. Headers are written `name: value`, in the order sent and with the case sent, and repeated headers stay separate.

A request with a body also has `body`: all of it, as text decoded from Latin-1, which keeps every byte. `proxy_connect` is the `CONNECT` that opened the first tunnel, with the same keys as an HTTP/1.1 request. A connection without TLS, over plain HTTP, has no `tls`.

### What stays, and what does not

Whatever a server can observe and a client decides stays as it was sent, in the order it was sent. Everything else follows these rules:

| Part | In the normal form | Why |
|---|---|---|
| Order of TLS extensions | Sorted: GREASE first, then by number | Chrome permutes them on every connection |
| GREASE values in cipher suites, extensions, groups, versions, signature algorithms and key shares | The placeholder `GREASE` | Chrome picks them at random |
| GREASE HTTP/2 settings and frames | The placeholder `GREASE`, with no value, flags or payload | The same |
| Order of QUIC transport parameters | Sorted: GREASE first, then by number | Chrome permutes them on every connection |
| GREASE transport parameters | The placeholder `GREASE`, with no value or length | Chrome picks all of those at random |
| GREASE version in `version_information` | The placeholder `GREASE`, first among the other versions | Chrome picks it at random, and puts it at a random place |
| GREASE HTTP/3 settings, frames and stream types | The placeholder `GREASE`, with no value, payload or stream number | Chrome picks them at random |
| ClientHello random and session ID | Dropped, except the session ID's length | Random |
| Key shares | The group and the key's length | The key is random |
| Encrypted ClientHello (ECH) | The ClientHello type and the KDF and AEAD | The configuration ID, encapsulated key and payload are random, and so is the payload's size |
| `pre_shared_key`, `session_ticket`, `cookie`, `early_data`, `padding` | Presence only | Their content changes between connections, or follows from the rest of the ClientHello |
| QUIC connection IDs, in packets and in transport parameters | Their length | Random |
| `initial_round_trip_time` | Dropped | It holds what Chrome measured on an earlier connection, and only connections that had one before them have it |
| `debugging_sni`, `discard` | Presence only | They hold the server's name, or random bytes |
| The split of a ClientHello into packets and CRYPTO frames, and the PING and PADDING frames between them | Dropped | Chrome splits it at random, and how many packets it takes follows from sizes that are random too |
| Size of the first datagram | Counted with its IP and UDP headers | Chrome makes it 1250 bytes on IPv4 and 1230 on IPv6, which is the same size once they are counted, and which of the two it uses says nothing about Chrome |
| Server name | The placeholder `<host>` | It says where the server is, not who the client is |
| The server's name and port in header values and in a `CONNECT` line | The placeholders `<authority>` and, with the page's scheme, `<origin>`. For the other names, `<same-site-authority>`, `<same-site-origin>`, `<cross-site-authority>` and `<cross-site-origin>` | The same |
| The random part of a multipart boundary, in headers and bodies | `----WebKitFormBoundary<random>`, for that prefix and 16 alphanumerics. Any other boundary stays as sent | Chrome picks it at random |
| The value of `device-memory`, `dpr`, `viewport-width`, `rtt`, `downlink`, `ect` and of `sec-ch-device-memory`, `sec-ch-dpr`, `sec-ch-viewport-width`, `sec-ch-viewport-height`, `sec-ch-prefers-color-scheme`, `sec-ch-prefers-reduced-motion` and `sec-ch-prefers-reduced-transparency` | The placeholder `<machine>`, in its place | It says what the machine, its window, its network and its user are, and not which Chrome this is. So [a Persona claims a value of its own](persona.md#what-they-say). `rtt` and `downlink` also change between requests |
| HTTP/2 stream IDs and stream dependencies of requests and `later_frames` | Dropped | They depend on which requests happened to be open. In `preface` they stay, because no request has started yet |
| HTTP/3 stream IDs of requests | Dropped | The same. On unidirectional streams they stay, because a client opens those whatever it requests |
| HTTP/3 GOAWAY on the control stream | Dropped | It depends on when the client leaves |

A number with a known name is written as that name, such as `TLS_AES_128_GCM_SHA256`. Any other number stays in hexadecimal, such as `0x1234`, and an extension with no known structure keeps its content in hexadecimal too. If a known extension is malformed, its content shows under `malformed`. Naming another number later changes the normal forms that hold it, so those need recording again.

Only connections that carried a request of a Recording Case count, and a run's normal form looks only for that run's cases. Chrome also opens connections that never carry a request, and it can spread one page's requests over several connections. Those that count must all have the same ClientHello and preface, the same `quic` and `http3` over QUIC, or the recording has no normal form and the kit says so.

### What it does not capture yet

- **Which requests share a connection.** The normal form describes one connection and then each request, so it cannot show how a client spreads its requests over connections, such as [partitioning](baselines.md#features-on-in-chromes-own-code). The raw recording can.
- **ALPS.** Python's `ssl` cannot negotiate it, so the normal form has Chrome's offer but not the settings Chrome would then send, and no server here can send the `ACCEPT_CH` frame.
- **Resumed connections.** The server gives out no tickets, so the normal form describes fresh connections only.
- **Frames that depend on the response or on timing.** The normal form leaves out frames sent after the first request on stream 0, such as a later `SETTINGS` or `PING`. They remain in the raw recording.
- **Later ClientHellos.** After a HelloRetryRequest, only the first ClientHello is recorded.
- **QUIC after the Initial packets.** aioquic hides what the later packets hold apart from stream data, such as ACK frames, NEW_CONNECTION_ID frames and flow control updates. The kit sees none of it.
- **QPACK.** How a client encodes its headers, and what it says on its QPACK encoder stream, depends on the requests before them. Those bytes stay in the raw recording, but only the decoded headers are in the normal form.
- **Chaos in Initial packets.** The normal form cannot tell a client that splits its ClientHello at random from one that does not, because any one way of splitting it is random too.
- **Lines for Google's domains.** Chrome adds them by the name in the URL, and the names of the case pages are not Google's. See [What Chrome adds for Google's domains](#what-chrome-adds-for-googles-domains).

### What can differ between Chrome installations

A fresh user data directory has no variations seed, so it shows no field trials. [baselines.md](baselines.md) is how Baselines get one.

Chrome's trust anchors, in the `trust_anchors` extension, start from its compiled-in root store, and the PKI metadata component can replace them without a new **Chrome Release**. So we compared a Chrome that holds the component with the Baselines, using Chrome 157.0.8103.39 on 2026-11-06:

| Question | Answer | From |
|---|---|---|
| Does the list differ? | No. The component holds root store version 43 and Chrome compiles in 42, with the same 28 IDs | Measured |
| How often does it change? | The set of IDs has changed 8 times since IDs first appeared in July 2025, and last on 2026-08-17. The root store's version goes up about every three weeks, mostly without changing the set | The source's history |
| Is one fixed list per **Engine Release** better than the compiled-in one? | No. mitto keeps the compiled-in list | Both of the above |

#### What Chrome does

This part comes from reading Chromium 157's source. `SSLConfigServiceManager` starts with the IDs of the compiled-in root store. `PKIMetadataComponentInstallerService` reads the component's `crs.pb` when the component arrives and at each start. If its `version_major` is above the compiled-in one, the IDs in it replace the list for new connections. IDs are sorted before they go out, so only the set can differ. The first check for component updates comes a minute after a start, and the next every 5 hours.

#### What we measured

From a new user data directory, with the flags of a Baseline, `chrome://components` showed PKI Metadata at `0.0.0.0`, `Checking` after a minute, and `1825`, `Updated`, after 335 seconds. Its `crs.pb` has the same IDs as the Baselines. From that directory, [`record.ps1`](baselines.md#a-chrome-that-has-run-for-a-while) gave normal forms identical to `tests/baselines/157` in all nine files. That was three sessions straight after a start and two with Chrome settled first.

With the lists equal, that alone cannot show that the wire follows the component. So in a copy of that directory, we took the ID `d6790901` off its anchor in `crs.pb`. The anchor stays, so what Chrome trusts does not change. No real Chrome has this file.

| Session from the copy | Result |
|---|---|
| Chrome settled for 20 seconds before each page | All 28 connections sent 27 IDs. That ID is the only difference from the Baselines, in each of the eight files with TLS |
| Three starts, each with a new connection about every second for 5 minutes | The list went from 28 to 27 IDs 1.6 to 1.9 seconds after the start, and stayed there |
| Two sessions straight after a start | All 95 connections sent 28 IDs, because a recording is over by then |

So the first connections after every start of Chrome carry the compiled-in list, whatever the component holds. Baselines hold that list even from a directory that has the component, and a comparison has to let Chrome settle.

#### How often it changes

This part comes from the history of `net/data/ssl/chrome_root_store/root_store.textproto`:

| Root store version | In the source since | IDs | Change |
|---|---|---|---|
| 23 | 2025-07-23 | 4 | The first IDs |
| 24 | 2025-08-12 | 15 | 11 more |
| 25 | 2025-10-06 | 17 | 2 more |
| 29 | 2026-01-08 | 36 | 19 more |
| 31 | 2026-04-14 | 32 | 4 fewer |
| 34 | 2026-06-20 | 29 | 3 fewer |
| 35 | 2026-06-23 | 32 | The same 3 back |
| 38 | 2026-08-17 | 28 | 4 fewer |

Versions 39 to 45, the last on 2026-11-05, keep those 28. These are dates in the source. On 2026-11-06 the component held version 43, so what it serves can be behind. That is one sample, and we did not measure when a version reaches the component.

At a tag of each Chrome Release, the compiled-in store has 36 IDs in 145 to 147, 32 in 148 to 152, and 28 in 153 to 158. By the dates above, the newest stable Chrome Release's compiled-in list differed from the newest root store on 21 of the 282 days since 145 went stable on 2026-01-28: 9 days from version 31 until 148, the 3 days of version 34, and 9 days from version 38 until 153.

#### Why the compiled-in list stays

`SSLContextConfig::trust_anchor_ids` could state a fixed list per **Session**, taken from the component when an Engine Release is made. That would not be better:

- Today it would be the same list. It only differs if the set changes after the Engine Release's branch, and a new Engine Release follows every two weeks.
- It stops following the component as soon as the release is made, as the compiled-in list does, so it falls behind a Chrome that has run for a while in the same way.
- It could name a root that the Session's verifier does not have. The verifier uses the compiled-in store, and mitto has no component to update it. A server with a chain for that ID may send that chain, and the Session then refuses it.
- Baselines hold the compiled-in list, so a Parity Check would need a **Known Gap** for a difference we chose.

A Session therefore looks like a Chrome in the first seconds after a start. When the set does change, it differs from a Chrome that has run for a while until the Engine Release moves past that change.

#### What this does not cover

The Chrome here ran for minutes, not days, and no component with a different set of IDs has reached it. That a component replaces the list while Chrome is running, without a new start, comes from the source.

Merkle Tree Certificates would change the answer. With `VerifyMTCs` on, Chrome adds the IDs of its Merkle Tree CAs, and a second component, PKI Metadata Fastpush, adds landmark IDs that expire. The feature is off by default in 157 and upstream on 2026-11-06, the seeds of 2026-11-05 and 2026-11-06 have no study that names it, and `chrome://components` showed no such component. A Baseline would show it, because it adds IDs to the compiled-in list too.

## What Chrome adds for Google's domains

Real Chrome adds request lines for Google's domains that a **Session** does not send, so a server there can tell the two apart. No **Baseline** holds them, because the names of the capture server are not Google's. A Session goes on sending none of them, as [parity.md](parity.md#googles-domains) says.

| Line | Goes to | Its value | Can a Session state it? |
|---|---|---|---|
| `x-browser-channel` | [The domains](#the-domains) and [requests](#the-requests) below | `stable` | Yes. It is the same in the whole build |
| `x-browser-year` | The same | `2026` in 155, 156 and 157 | Yes. The build sets it, not the clock |
| `x-browser-copyright` | The same | `Copyright 2026 Google LLC. All Rights Reserved.` in 155, 156 and 157 | Yes. It is the same in the whole build |
| `x-browser-validation` | The same | 28 characters, one value per **Chrome Release** | No. It hashes a seed and an API key inside Chrome with Chrome's `User-Agent`. **Known Gap** |
| `x-sec-ch-ua-not-a-border` | The same, from Chrome 156 on | 32 lowercase letters, which change every five minutes | No. A closed library inside Chrome makes it. **Known Gap** |
| `x-client-data` | The same domains, over HTTPS only | Variation IDs, which differ from one start of Chrome to the next | No. It follows a user's field trials, and a Session has no seed. **Known Gap** |
| `x-chrome-id-consistency-request` | `accounts.google.com` only | Fixed fields, Chrome's OAuth client ID and a device ID per user data directory | Only in part, and it asks Google for what only Chrome's account code can use. **Known Gap** |

The first three alone would not help. Chrome sets those three and `x-browser-validation` together, so a request with only the three is one that no Chrome sends. Which requests get what is the same in 155, 156 and 157, except that 155 has no `x-sec-ch-ua-not-a-border`.

### What Chrome does

This part comes from reading Chromium's public source, at 157.0.8103.27 and the tags of 155 and 156. The names of the lines are not in it, which is why a search for them finds nothing.

`RequestHeaderIntegrityURLLoaderThrottle` in `chrome/common/request_header_integrity/` adds the lines. Only builds with Google's branding have it (`enable_request_header_integrity = is_chrome_branded`). `chrome/browser/chrome_content_browser_client.cc` and `chrome/renderer/url_loader_throttle_provider_impl.cc` give it to what goes through URL loader throttles in the browser and in a page. If `google_util::IsGoogleAssociatedDomainUrl()` holds for the URL, it sets the lines. On a redirect it sets them again for a name that passes, and removes them for one that does not. Nothing there looks at the profile or the scheme.

| Line | What the source says |
|---|---|
| `x-browser-channel` | `chrome::GetChannelName()` |
| `x-browser-year` | `LASTCHANGE_YEAR`, from `internal/build_derived_values.h`, when Chrome is built |
| `x-browser-copyright` | `CHROME_COPYRIGHT`, from the same place |
| `x-browser-validation` | Base64 of the SHA-1 of `kIntegritySeed`, `google_apis::GetAPIKey()` and `embedder_support::GetUserAgent()`. The seed is in `internal/integrity_seed_internal.h` |
| `x-sec-ch-ua-not-a-border` | `ChromeCompaneroHost` calls `GetHeaderName` and `GetCompaneroValue` in `chromecompaneros.dll`, a library that comes with Chrome, and hands it the same seed, API key and `User-Agent`. Each process keeps the value and asks for a new one every minute |

`internal` is a submodule of closed code, and the names of the lines are there too. The public files have placeholders such as `X-Placeholder-3`, and the name of the last line comes from the library at run time. So that this is the line of the library is our reading: it comes where that code sets its line, it is missing just after a start, and it follows the `User-Agent`. `PlatformRuntimeHost` can also run a library from a component called Platform Runtime, in a process of its own, every 3 minutes, and the values it returns overwrite those of the lines above.

`x-client-data` comes from elsewhere: `components/variations/net/variations_http_headers.cc`, which checks the same `IsGoogleAssociatedDomainUrl()` and asks for HTTPS and not Incognito. It only starts with a request, so it does not get added on a redirect into Google's domains. Its value is a `ClientVariations` message holding the Google-visible IDs of the field trials this Chrome drew, with fewer of them when a page that is not Google's made the request. `x-chrome-id-consistency-request` comes from `components/signin/core/browser/dice_header_helper.cc`, for `accounts.google.com`.

`IsGoogleAssociatedDomainUrl()` in `components/google/core/common/google_util.cc` has the same lists in the three Chrome Releases. It takes these names, on any port:

- `google.TLD` and `youtube.TLD`, with any subdomain, for the TLDs on the lists in `google_tld_list.h`;
- any subdomain of `android.com`, `doubleclick.com`, `doubleclick.net`, `ggpht.com`, `googleadservices.com`, `googleapis.com`, `googlesyndication.com`, `googleusercontent.com`, `googlevideo.com`, `gstatic.com`, `litepages.googlezip.net`, `youtubekids.com` and `ytimg.com`, but not those names themselves;
- exactly `googleweblight.com`.

### What we measured

This part was measured on 2026-11-07 with stable Windows Chrome 155.0.8059.85, 156.0.8078.80 and 157.0.8103.39, windowed, from new user data directories with [the flags of a Baseline](baselines.md#what-it-does), through net logs at their default capture mode, which hides cookies. Nothing signed in.

| Runs | How | What they are for |
|---|---|---|
| Two sessions per Chrome Release at real domains, 90 seconds each | `www.google.com`, `www.youtube.com`, `accounts.google.com`, a font's CSS at `fonts.googleapis.com`, and `example.com` with an image, script, frame and `fetch()` from Google's domains. Also a new start of one directory, and a first start with no seed | What real Google pages cause. 1,232 of 1,521 requests had the lines |
| One session per Chrome Release with every name at a loopback server | `--host-resolver-rules` sends every name to a server on the machine, so no request left it. 78 names, and about 30 kinds of request, from pages at Google's domains and elsewhere | Which names and requests get the lines, without the traffic that would take |
| `www.google.com` at the capture server, for each Chrome Release and protocol | `--host-resolver-rules` and `--origin-to-force-quic-on`, as in [a recording](baselines.md#what-it-does) | The lines in a normal form, on the wire, next to the Baselines |
| Chrome 157 with every name at a loopback server, one session each | `--user-agent`, `--accept-lang`, an Incognito window, Chrome for Testing, and sessions of 12 and 22 minutes in Chrome 156 and 157 | What changes the values |

Runs at a loopback server or at the capture server are not Baselines, and none of this went into `tests/baselines`. The raw net logs stayed out of the repository too, because they can hold session data.

#### The domains

Of the 78 names, the 29 that the rule above takes had the lines on a navigation and on a `fetch()` from that page, and the 49 others had none. Those include `gstatic.com` and `googleapis.com` themselves, `www.youtube-nocookie.com`, `www.googletagmanager.com`, `www.google-analytics.com`, `www.chromium.org`, `about.google`, `www.google.xyz` and `www.google.com.example`. `x-client-data` went to the same names. All three Chrome Releases gave the same answer for all 333 requests of those sessions, apart from `x-sec-ch-ua-not-a-border`, so a port of that one function would be enough to choose the names.

#### The requests

| Request to one of those domains | The `x-browser-*` lines and `x-sec-ch-ua-not-a-border` | `x-client-data` |
|---|---|---|
| Navigations: from the address bar, a link, a script, a form, a reload, a frame | Yes | Yes |
| Script, image, style, font, video, `fetch()` and XHR of any method and mode, beacon, `preload`, `prefetch`, `EventSource`, the script of a worker or service worker, and a `fetch()` inside one | Yes | Yes |
| The same from a page that is not Google's | Yes | Yes, with fewer IDs |
| A hop of a redirect, into one of those domains | Yes | Only if the chain began at one of them |
| A hop of a redirect, away from them | Removed | Removed |
| Plain HTTP | Yes | No |
| Another port | Yes | Yes |
| A CORS preflight, a WebSocket handshake | No | No |
| Chrome's own requests, such as for a seed or updates | No | On some |

#### Their place

The lines come after everything the request has so far and just before `sec-fetch-site`, in this order, with the case that HTTP/1.1 sends:

```text
X-Browser-Channel
X-Browser-Year
X-Browser-Validation
X-Browser-Copyright
X-Sec-CH-UA-Not-A-Border
X-Client-Data
```

Over HTTP/2 and HTTP/3 they are in lowercase. Next to the Baselines of each Chrome Release, over all three protocols and for navigation, `fetch()`, script and image, these lines were the only difference. The connection was the same, and the other lines were the same and in the same order.

A navigation that starts with Chrome has no `x-sec-ch-ua-not-a-border`, because the value is not there yet. All 8 of those in Chrome 156 and 157 lacked it, while the requests of the same page had it. The 5 that [started over for ALPS](parity.md#starting-over) at `www.google.com` had it after `x-client-data`, which fits the second try finding it there.

#### Their values

| Line | New directories | New starts | Chrome Releases | With `--user-agent` |
|---|---|---|---|---|
| `x-browser-channel`, `x-browser-year`, `x-browser-copyright` | The same | The same | The same | Not read |
| `x-browser-validation` | The same | The same, with or without a seed | Different in each | Different. `--accept-lang` makes no difference |
| `x-sec-ch-ua-not-a-border` | The same at the same time | The same at the same time | Different in each | Different |
| `x-client-data` | Different | Different. None in a first start, with no seed | Different | Not compared |
| `device_id` in `x-chrome-id-consistency-request` | Different | Not read | Different | Not read |

The other fields of `x-chrome-id-consistency-request` were the same everywhere: `version=2`, the client ID, `signin_mode=all_accounts` and `signout_mode=show_confirmation`. It went with every request of the page at `accounts.google.com`, 9 per session, and not with Chrome's own request to that name.

`x-sec-ch-ua-not-a-border` follows the clock. In 22 minutes of Chrome 157 and 12 of Chrome 156, with a request every 10 seconds, it changed 6 times, each time within a minute after a time that ends in :00 or :05 UTC, and never in between. Each process kept its old value for up to a minute more, as the source says, so requests from two processes differed around that time. In one such period, four new directories of Chrome 157 had the same value. `x-browser-validation` did not change during these runs.

A real Incognito window sent the `x-browser-*` lines and `x-sec-ch-ua-not-a-border`, with the same `x-browser-validation`, and no `x-client-data`. Chrome for Testing 157.0.8103.39, which has no Google branding, sent none of them. A Chrome 157 left running for 15 minutes, which then went to `www.google.com`, had the same `x-browser-validation` and no other new line.

Chrome 156 on macOS, headless and from new directories, sent these lines too, with other values of `x-browser-validation` and `x-sec-ch-ua-not-a-border`. Its navigation had another `x-browser-validation` than the requests of the page, with the same `User-Agent` line, which no windowed Chrome here did. This shows that the values follow the operating system or headless mode, and not which.

### What this means for a Session

`x-browser-validation` is the same for every request of a Chrome Release, so a recorded value would look right for that Chrome Release's `User-Agent`. It would still be the output of a seed and a key inside Chrome, so it stays a Known Gap. We did not look for the seed or the key, and did not open Chrome's binary or the library. `x-sec-ch-ua-not-a-border` could not be recorded at all, because it changes. A Session without it looks like the first navigation after a start of Chrome, and like nothing after that.

A Session without `x-client-data` looks like a Chrome with no seed yet, or an Incognito window. Sending one recorded value would look like one draw of one seed, which goes out of date as studies change.

So a Session sends none of these lines. That is what a build without Google's branding sends, although a Session's `sec-ch-ua` says Google Chrome, so a server at these domains that reads these lines can tell a Session from a real Chrome. On every other domain, nothing here changes.

### What this does not cover

- A second machine. By the source, two machines get the same `x-browser-validation`, because the hash has the same inputs. The values on macOS above differ, but that Chrome differs in more than its machine.
- The Platform Runtime component. After 15 minutes, that Chrome had other components, but none by this name in `chrome://components` and no `PlatformRuntime` directory. If it reaches a Chrome, the source says that its library can change these values. We have not seen that.
- HTTP/3 at Google's own servers. The real sessions went over HTTP/2, so HTTP/3 comes from the capture server alone.
- Why Chrome 155 sends no `x-sec-ch-ua-not-a-border`. Its source has the same code, and it comes with a library of that name.
- Whether Google does anything with these lines, or answers differently without them. This page says what Chrome sends.
- `x-browser-year` and `x-browser-copyright` in another year. All three builds say 2026, so a change of year has not been seen, and the source says it follows the build.
- Other lines in the source that only apply with a policy, on Android or in a feature that a new directory does not use, such as `X-Goog-*` in Chrome's own requests, `YouTube-Restrict` and `X-CCT-Client-Data`. Lines that a page's own scripts set, such as `x-youtube-client-name`, are not Chrome's.

## From Python

A Parity Check can run the server itself. It picks free ports unless told otherwise, and it listens as long as the `with` block runs. The routes into it are `HTTP1`, `HTTP2`, `HTTP3`, `PLAIN_HTTP`, `PROXIED_HTTP1` and `PROXIED_HTTP2`, each with a recording of its own:

```python
from tools.capture import HTTP2, KIND_PATHS, CaptureServer, RequestKind, dumps_normal_form, normal_form

with CaptureServer() as server:
    url = f"https://{server.authority(HTTP2)}{KIND_PATHS[RequestKind.FETCH]}"
    ...  # Make the request to url, with server.spki_hash trusted.
    form = normal_form(server.recording(HTTP2))
print(dumps_normal_form(form))
```

`normal_form` includes only the Recording Cases in the recording, so a recording of one request gives a normal form with one entry in `requests`. Its second argument limits it to the cases of a run. It raises `RecordingError` when a recording has no normal form.

To reach the server over HTTP/3, a client needs `server.spki_hash` trusted and `server.authority(HTTP3)` in its list of origins to force QUIC on. For a run, `server.origins_to_force_quic_on(run)` gives that list, with the names of the case pages.

To reach a case page, a client needs `server.host_resolver_rules`, which a native Session takes as `host_resolver_rules` from `mitto._native`. `CASE_HOST` names the page, and `server.url(route, host=CASE_HOST, path=...)` and `server.recording(route, host=CASE_HOST)` go with it, so that the placeholders are the ones the Baselines have. `RUNS` lists every run, and `server.start_url(run)` and `server.chrome_flags(run)` say how to start one.

## Tests

`uv run pytest tests/capture` runs the kit's tests, including its fuzz targets on a fixed set of inputs. Files under `tests/capture/data` hold a real recording of Chrome, which the tests replay.

To run the same check against a real Chrome, headless, from fresh user data directories, name its executable in `MITTO_CHROME`. It goes through every run twice and checks that each **Recording Case** is what it says it is. Without that, the test is skipped:

```bash
MITTO_CHROME="/path/to/chrome" uv run pytest tests/capture/test_chrome.py
```

Every parser we write has a fuzz target in `tests/capture/test_fuzz.py`. They take arbitrary bytes, built mostly from mutated real traffic and well-formed structures with random content, and may only fail in the way the parser documents. To look for new failures, with new inputs and many more of them:

```bash
uv run pytest tests/capture/test_fuzz.py --hypothesis-profile=fuzz
```
