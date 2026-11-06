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
| HTTP/2 stream IDs and stream dependencies of requests and `later_frames` | Dropped | They depend on which requests happened to be open. In `preface` they stay, because no request has started yet |
| HTTP/3 stream IDs of requests | Dropped | The same. On unidirectional streams they stay, because a client opens those whatever it requests |
| HTTP/3 GOAWAY on the control stream | Dropped | It depends on when the client leaves |

A number with a known name is written as that name, such as `TLS_AES_128_GCM_SHA256`. Any other number stays in hexadecimal, such as `0x1234`, and an extension with no known structure keeps its content in hexadecimal too. If a known extension is malformed, its content shows under `malformed`. Naming another number later changes the normal forms that hold it, so those need recording again.

Only connections that carried a request of a Recording Case count, and a run's normal form looks only for that run's cases. Chrome also opens connections that never carry a request, and it can spread one page's requests over several connections. Those that count must all have the same ClientHello and preface, the same `quic` and `http3` over QUIC, or the recording has no normal form and the kit says so.

### What it does not capture yet

- **Which requests share a connection.** The normal form describes one connection and then each request, so it cannot show how a client spreads its requests over connections, such as [partitioning](baselines.md#features-on-in-chromes-own-code). The raw recording can.
- **ALPS.** Python's `ssl` cannot negotiate it, so the normal form has Chrome's offer but not the settings Chrome would then send.
- **Resumed connections.** The server gives out no tickets, so the normal form describes fresh connections only.
- **Frames that depend on the response or on timing.** The normal form leaves out frames sent after the first request on stream 0, such as a later `SETTINGS` or `PING`. They remain in the raw recording.
- **Later ClientHellos.** After a HelloRetryRequest, only the first ClientHello is recorded.
- **QUIC after the Initial packets.** aioquic hides what the later packets hold apart from stream data, such as ACK frames, NEW_CONNECTION_ID frames and flow control updates. The kit sees none of it.
- **QPACK.** How a client encodes its headers, and what it says on its QPACK encoder stream, depends on the requests before them. Those bytes stay in the raw recording, but only the decoded headers are in the normal form.
- **Chaos in Initial packets.** The normal form cannot tell a client that splits its ClientHello at random from one that does not, because any one way of splitting it is random too.

### What can differ between Chrome installations

A fresh user data directory has no variations seed, so it shows no field trials. [baselines.md](baselines.md) is how Baselines get one.

Chrome's trust anchors, in the `trust_anchors` extension, start from its compiled-in root store. When the PKI metadata component brings a root store with a higher version, Chrome takes the list from that, without a new Chrome Release. A recording is over before the component arrives, so Baselines hold the compiled-in list and cannot show what a Chrome that has run for a while sends.

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
