# The capture kit

The capture kit records what a client sends to a server: the raw TLS ClientHello, the Initial packets of QUIC, the decrypted HTTP/3 and HTTP/2 frames or HTTP/1.1 requests, a `CONNECT` to its proxy, and a **normal form** of them that compares equal between recordings of the same client. The normal form is also the **Baseline** file format, and **Parity Checks** compare in it. It also records a page's **Script Fingerprint** in a real Chrome, and writes [its **Normal Form**](#the-script-fingerprint). Terms in **bold** are defined in [CONTEXT.md](../CONTEXT.md).

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
| `recordings/documents/*.json` | The normal form of [what a document requests](#documents) beside the main Request Kinds |
| `recordings/*.raw.json`, `recordings/cases/*.raw.json` | The raw recording of each: every connection that came in by the same way, with the client's address, its ClientHello bytes, its Initial packets over QUIC, every frame and every request |
| `recordings/script/page.json` | The [Normal Form of the Script Fingerprint](#the-normal-form-of-a-script-fingerprint) of a top-level page |
| `recordings/script/page.raw.json` | What that page posted, with its address and the address of each client that reached it. It holds the values of the machine that recorded it, so it stays on that machine |
| `recordings/runs.json` | The name, address and flags of each run, and whether it records the wire or a Script Fingerprint, for a script |

To check that a client records the same twice, record it into two directories and compare the files.

`--tickets` has the server give out session tickets, and takes clients through [the runs that resume](#how-the-server-does-it) instead of the others.

`--host` sets another name ending in `.localhost`. `--http1-port`, `--http2-port`, `--http3-port`, `--plain-http-port`, `--proxy-port` and `--script-port` set other ports. The normal form does not depend on any of them. `--runs` names the runs to wait for, such as `--runs http1 http2 http3`.

`--click-in USER_DATA_DIRECTORY` does the clicking, in a Chrome with that directory and `--remote-debugging-port=0`. A navigation counts as the user's only with a user gesture, and Chrome sends `Sec-Fetch-User` only then, so a page cannot click for itself. This runs the page's own click on the page's own element through `Runtime.evaluate` with `userGesture`, which goes around the window's input. Every such call gives the page a user activation, whatever it runs, which lasts for seconds (`ThreadDebuggerCommonImpl::beginUserGesture()` in Blink), and a navigation that the page's own script then starts would count as the user's too. So it asks whether there is something to click without a gesture, and gives one only to the click. In headless Chrome 156 on macOS, real mouse events through `Input.dispatchMouseEvent` gave identical normal forms over all three protocols. It never attaches to the page of a Script Fingerprint, which needs no click.

## What the server does

- It serves at a `*.localhost` name. Chrome and the network stack resolve these names to loopback themselves, with no DNS or hosts file entry, and unlike an IP address such a name gets SNI. The server listens only on loopback.
- It also answers under three names that are [not local](#recording-cases), which a client has to be told to send to loopback. Its one certificate names them all, so a client may pool their connections over HTTP/2 and HTTP/3.
- It serves HTTP/1.1 and HTTP/2 on separate TCP ports, and HTTP/3 on a UDP port. Each port offers one protocol through ALPN. On a TCP port, a client that does not take it is served HTTP/1.1, and the normal form shows that. Over QUIC, such a client gets no connection.
- It serves plain HTTP/1.1 on another TCP port.
- It serves the Script Fingerprint's pages on a TCP port of their own, over HTTP/1.1 with TLS, so that the record such a page posts, over a megabyte, stays out of the wire's raw recordings.
- It is an HTTP proxy on another TCP port. It answers `CONNECT` to one of its names on its HTTP/1.1 or HTTP/2 port by serving that port's protocol in the tunnel, on the same socket, so a tunnel reaches nothing outside the server. It refuses anything else with a 403. What comes through it gets its own recording, whose connections also hold the `CONNECT` that opened them.
- It gives out no session tickets unless it is told to with `--tickets`. Every connection is therefore a fresh one, with no `pre_shared_key`, including the later connections of one page load. Baselines come from a server like this, and [they need nothing else](#resumed-connections-and-alps).
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
| Documents over HTTP/1.1, HTTP/2, HTTP/3 | `documents/http1.json`, `documents/http2.json`, `documents/http3.json` | The port of that protocol | [Those below](#documents) |
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

Every response under `/case/` allows every origin, because a request shows no sign of that and a page cannot go on without it. So the cases need no preflight, and each hop of a redirect also works with `Origin: null`. Only [the runs of documents](#documents) have a preflight, and answers for requests with credentials.

### Documents

The runs at `documents/` record what a document requests beside the main Request Kinds, and the navigations that it starts by itself: the Request Kinds that M8's pages make. Their files stay apart from `cases/`, because the Parity Checks of the wire replay each case in `cases/`, and they do not replay these yet. A recording session takes these runs after those above and before the client hints, whose asking would reach them. The three runs over TLS go through these pages in order, from `/case/documents-start`. Only the forms need a click. The page starts each other navigation by itself, with no user activation:

| Recording Case | What makes it |
|---|---|
| `style`, `style_import`, `style_cross_site` | Stylesheets of the first page, the first of which `@import`s another |
| `font`, `font_cross_site` | The fonts of that stylesheet's `@font-face` rules, which text of the page uses. The page lays the text out and waits for them. Their answer is no font, which Chrome finds out only after asking |
| `module`, `module_static_import`, `module_dynamic_import`, `module_cross_site` | A module script that imports a second one statically and a third with `import()`, and one at another site |
| `xhr_get`, `xhr_get_cross_site`, `xhr_post_text`, `xhr_get_sync`, `xhr_post_text_sync` | `XMLHttpRequest`, asynchronous and synchronous, without a body and with a string |
| `worker`, `worker_import_scripts`, `worker_fetch`, `worker_module` | A classic dedicated worker, which calls `importScripts()` and `fetch()`, and a module worker |
| `beacon_text`, `beacon_blob`, `beacon_form_data`, `beacon_text_cross_site` | `sendBeacon()` with a string, a `Blob` with no type and a `FormData` |
| `fetch_later` | `fetchLater()`, whose request Chrome sends when the first page goes |
| `preflight`, `fetch_post_json_cross_site` | `fetch()` with JSON to another site, and the preflight that Chrome sends first. Both go to one path, and each counts by a line that the other lacks, `Access-Control-Request-Method` and `Content-Type` |
| `fetch_credentials_omit`, `fetch_credentials_same_origin`, `fetch_credentials_include` | `fetch()` with each `credentials` mode, to the other origin of the site, which has just set a cookie for each of these paths. A cookie of the same site is no third-party cookie, so Chrome's choice about those does not decide what goes |
| `fetch_no_cors` | `fetch()` with `mode: "no-cors"` to another site |
| `navigation_location`, `navigation_click`, `navigation_meta_refresh` | A navigation that the page starts through `location`, a link's `click()` and `<meta http-equiv="refresh">`, each from the page before |
| `redirect_location_1`, `redirect_location_2` | One through `location`, whose hop goes to another site |
| `frame_same_origin`, `frame_same_site`, `frame_cross_site` | The navigation of a frame at each origin |
| `frame_script`, `frame_image`, `frame_fetch` | What the frame of the other site asks for: a script and an image of its markup, and a `fetch()` |
| `document_style`, `document_preload_style`, `document_preload_script`, `document_preload_image`, `document_preload_font`, `document_script`, `document_image_small`, `document_image`, `document_style_body`, `document_script_deferred` | What a document's markup asks for: stylesheets in its head and its body, preloads of a stylesheet, a script, an image and a font, a script that blocks the parser and a deferred one, and an image that says it is small and one that does not say its size |
| `favicon` | `/favicon.ico`, which Chrome asks for by itself after that document has loaded, because it names no icon. Every other page names a `data:` icon. The page waits three seconds and then says that Chrome did not ask, which counts only if Chrome did not, as `{"refused": true}` |
| `navigation_form_get`, `navigation_form_text`, `navigation_form_cross_site` | A click on the button of a form that GETs, which puts its fields in the query that stands for the case, one that POSTs `text/plain`, and one that POSTs urlencoded to another site |
| `navigation_history_back` | `history.back()` to the page of an earlier step, which Chrome asks for again. The page in between sets a cookie for that page's path, so only this request carries it |

The answer to the preflight allows `Content-Type`. Those to requests with credentials allow the page's origin with credentials, which an answer that allows every origin does not.

Chrome 157 keeps a page whose answer says `Cache-Control: no-store` in its back/forward cache, for three minutes, and restores it unless its cookies changed (`kCacheControlNoStoreEnterBackForwardCache`, on by default at the level `kStoreAndRestoreUnlessCookieChange`). It keeps none whose scripts got a `no-store` answer. The page that the traversal goes back to is both, so Chrome asks for it again. A page that comes back from the cache says so, and the case is then missing. Cookies do not go by port, so that page also takes its cookie away again, and the run over the next protocol starts without it.

`order` in [the normal form](#the-normal-form) lists the ten cases of the document's markup in the order Chrome opened their streams, over HTTP/2 and HTTP/3, where one connection carries them. Over HTTP/1.1 Chrome spreads them over connections, so the order in which they arrive is not Chrome's, and the normal form has none.

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
| `requests` | One entry per Recording Case recorded, always in the order navigation, fetch, script, image and then those of [the tables above](#recording-cases). If a case was requested twice, the first request counts. A case that has to carry a line only counts a request with it, so the first request with one counts: `Cookie` for `cookie` and `navigation_history_back`, `If-None-Match` for `revalidation`, `Access-Control-Request-Method` for `preflight` and `Content-Type` for `fetch_post_json_cross_site`. |
| `order` | Only in [the files of documents](#documents) over HTTP/2 and HTTP/3. The names of the cases of the document's markup, in the order the client opened their streams. |

An HTTP/1.1 request has `request_line` and `headers`. An HTTP/2 request has the `flags` of its HEADERS frame, the frame's `priority` if it has any, `headers` and, if there were any, `later_frames`: what the client sent on the stream after the headers, without the content of bodies or encoded header blocks. An HTTP/3 request has `headers` and, if there were any, `priority_updates`: the priorities that PRIORITY_UPDATE frames on the control stream gave it, in order. It also has `earlier_frames` and `later_frames`: what the client sent on the stream before and after the first headers, without the content of bodies or encoded header sections. On a connection that sent early data it has `early_data`, which lists what came before the client's Finished: `request` for the request itself, and the type of each other stream. It goes with the request, because what a client sends early depends on the request. Headers are written `name: value`, in the order sent and with the case sent, and repeated headers stay separate.

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
| The value of `device-memory`, `dpr`, `viewport-width`, `rtt`, `downlink`, `ect` and of `sec-ch-device-memory`, `sec-ch-dpr`, `sec-ch-viewport-width`, `sec-ch-viewport-height`, `sec-ch-prefers-color-scheme`, `sec-ch-prefers-reduced-motion`, `sec-ch-prefers-reduced-transparency` and `sec-ch-ua-platform-version` | The placeholder `<machine>`, in its place | It says what the machine, its window, its network, its Windows and its user are, and not which Chrome this is. So [a Session claims the value of its own Machine](persona.md#what-they-say). `rtt` and `downlink` also change between requests. [Their form](#the-form-of-a-hidden-value) is checked instead |
| HTTP/2 stream IDs and stream dependencies of requests and `later_frames` | Dropped | They depend on which requests happened to be open. In `preface` they stay, because no request has started yet |
| HTTP/3 stream IDs of requests | Dropped | The same. On unidirectional streams they stay, because a client opens those whatever it requests |
| HTTP/3 GOAWAY on the control stream | Dropped | It depends on when the client leaves |

A number with a known name is written as that name, such as `TLS_AES_128_GCM_SHA256`. Any other number stays in hexadecimal, such as `0x1234`, and an extension with no known structure keeps its content in hexadecimal too. If a known extension is malformed, its content shows under `malformed`. Naming another number later changes the normal forms that hold it, so those need recording again.

Only connections that carried a request of a Recording Case count, and a run's normal form looks only for that run's cases. Chrome also opens connections that never carry a request, and it can spread one page's requests over several connections. Those that count must all have the same ClientHello and preface, the same `quic` and `http3` over QUIC, or the recording has no normal form and the kit says so.

### The form of a hidden value

A value hidden as `<machine>` cannot be compared, but how it is written can. [`tools/capture/machine_forms.py`](../tools/capture/machine_forms.py) holds one form for each of those hints: what Chrome on Windows can send, by [what decides each value](persona.md#what-decides-each-value), such as `2`, `4`, `8`, `16` or `32` for `device-memory`, a multiple of 0.05 from 0 to 10 in its shortest decimal for `downlink`, and a version in quotes such as `"19.0.0"` for `sec-ch-ua-platform-version`. [`tests/test_machine.py`](../tests/test_machine.py) holds every value a Session can work out from its Machine to those forms, and this holds a recording's:

```bash
uv run python -m tools.capture.machine_forms recordings/a recordings/b
```

It reads every raw recording under each directory, `cases` included, and prints for each directory and hint the form, how many values had it and whether all did, and never a value. A directory fails when a value lacks its form, or when it has no value to check at all. The values are those of the machine that recorded them, so it runs where the recordings are kept, and [baselines.md](baselines.md#check-and-commit) says when.

### What it does not capture yet

- **Which requests share a connection.** The normal form describes one connection and then each request, so it cannot show how a client spreads its requests over connections, such as [partitioning](baselines.md#features-on-in-chromes-own-code). The raw recording can.
- **ALPS.** Python's `ssl` cannot negotiate it, and neither can aioquic, so the normal form has a client's offer only, and no server here can send the `ACCEPT_CH` frame. Over TCP and over HTTP/3 [that offer is enough](#resumed-connections-and-alps), because what a client then sends is empty. Our check of that over HTTP/3 was by hand, and it is not in the kit.
- **Resumed connections, beyond a ticket.** With `--tickets` the kit records them, and [Baselines do not need them](#resumed-connections-and-alps). Still out of reach: early data over TCP, because Python's `ssl` cannot say that a ticket allows it; a server that refuses early data or a ticket, or answers `425 Too Early`; a resumption after a HelloRetryRequest; and the token in an Initial packet, because aioquic sends no `NEW_TOKEN` frame, so `token_length` stays 0 here where a real server makes it more.
- **Frames that depend on the response or on timing.** The normal form leaves out frames sent after the first request on stream 0, such as a later `SETTINGS` or `PING`. They remain in the raw recording.
- **Later ClientHellos.** After a HelloRetryRequest, only the first ClientHello is recorded.
- **QUIC after the Initial packets.** aioquic hides what the later packets hold apart from stream data, such as ACK frames, NEW_CONNECTION_ID frames and flow control updates. The kit sees none of it.
- **QPACK.** How a client encodes its headers, and what it says on its QPACK encoder stream, depends on the requests before them. Those bytes stay in the raw recording, but only the decoded headers are in the normal form.
- **Chaos in Initial packets.** The normal form cannot tell a client that splits its ClientHello at random from one that does not, because any one way of splitting it is random too.
- **Lines for Google's domains.** Chrome adds them by the name in the URL, and the names of the case pages are not Google's. See [What Chrome adds for Google's domains](#what-chrome-adds-for-googles-domains).
- **The ClientHelloInner of real ECH.** The names of the case pages are never looked up, so Chrome finds no HTTPS record and sends ECH GREASE. With real ECH the outer ClientHello has the same normal form, so only the inner one is missing. See [Real ECH](#real-ech).

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

Merkle Tree Certificates would change the answer. With `VerifyMTCs` on, Chrome adds the IDs of its Merkle Tree CAs, and a second component, PKI Metadata Fastpush, adds landmark IDs that expire. The feature is off by default in 157 and upstream on 2026-11-06, the seeds of 2026-11-05 to 2026-11-07 have [no study that names it](baselines.md#merkle-tree-certificates), and `chrome://components` showed no such component. A Baseline would show it, because it adds IDs to the compiled-in list too.

## The Script Fingerprint

The kit also records the **Script Fingerprint** of a real Chrome: what [the probe](runtime.md#the-probe) gives in a page. [baselines.md](baselines.md#the-script-fingerprint) says how a recording session records it with the wire, and a Parity Check of a Page compares with its Normal Form.

### Its Recording Case

The first **Recording Case** is a top-level page, `/script/page` at `capture.test`, which [#207](https://github.com/darionxt/mitto/issues/207) chose: an HTTPS page at the case pages' name, which a page at `127.0.0.1` matches but for its origin, and which a plain `http://` page does not, since that is no secure context and has 250 fewer globals. Frames and workers come with M8. The run is `script/page`, the first in `runs.json`, and its start of Chrome has the flags of a case page: `--ignore-certificate-errors-spki-list`, `--host-resolver-rules` and `--no-proxy-server`. The first two make Chrome show its infobar of unsupported flags, which takes height from the viewport, and [the Normal Form](#the-normal-form-of-a-script-fingerprint) hides that with the window's sizes.

The page holds the probe as a string. Once the load event is over, so that no event is being dispatched while the probe reads the window, it evaluates it as the Page program does, posts the record to `/script/record`, and says "Recorded". What it posts is the record's JSON text. The page sets nothing on the window, gives no element an id or a name, which would be properties of the window, and uses no regular expression, whose matches would change `RegExp`'s legacy statics, all of which the probe reads. Its markup is part of the case, since the probe reads `document.all`, the title and the like.

Its start of Chrome must not have `--remote-debugging-port=0`, which turns on Blink's `AutomationControlled` and makes `navigator.webdriver` true. `record.ps1` gives it a fixed port, which changes nothing a page sees, and reads `chrome://version` through it only once the page has posted its record. Nothing attaches to the page itself.

The route has a port of its own, `--script-port`, 8448 by default, over HTTP/1.1. The server's HTTP/2 sends no `WINDOW_UPDATE`, so a client can send it no more than 64 KiB. Over HTTP/1.1 it keeps a body of up to 16 MiB.

### The Normal Form of a Script Fingerprint

[`tools/capture/script_normal_form.py`](../tools/capture/script_normal_form.py) writes it. It is the probe's record with what the machine decides hidden as `<machine>`, what changes from run to run as `<run>` and the page's origin behind placeholders, written as `dumps_record()` writes a record, with each own property, getter's value, check and value of the probe's third step on a line of its own, so that two Normal Forms differ by lines. Chrome 157's takes 1.33 MB, within the 1.5 MB that a Baseline of the first Recording Case may take for each **Chrome Release**, which the owner chose on 2026-11-12. Two sessions of `record.ps1` with one Chrome give it byte for byte, and so does one with no debugging port at all ([baselines.md](baselines.md#the-script-fingerprint)).

It keeps everything else, because that is what a Page has to match: which interfaces and members there are, their order, attributes, names, `length`s and source text, the prototype chains, what each check throws, `window.chrome` and `console.memory` as they are, the deprecation reports, and every value that the Persona decides, such as `navigator.userAgent`, `platform`, `languages`, the brands and the high entropy values but the platform version, `plugins`, `webdriver`, the permission states of a new user data directory and the permissions policy's lists.

| Hidden as `<machine>` | Why |
| --- | --- |
| `innerWidth`, `innerHeight`, `outerWidth`, `outerHeight`, `screenX`, `screenY`, `screenLeft`, `screenTop`, `devicePixelRatio`; `width` and `height` of `visualViewport` and of `viewport.segments[0]`, with its `right` and `bottom` | The viewport, the window and the scale. The infobar of unsupported flags takes height from the viewport too |
| `screen`'s sizes, `availLeft` and `availTop`, `colorDepth`, `pixelDepth`, `isExtended`, and `screen.orientation`'s `type` and `angle` | The display, the work area, whether it has HDR, whether there are more displays and how the display is turned |
| `navigator`'s `hardwareConcurrency`, `deviceMemory`, `maxTouchPoints` and `cpuPerformance` | The processors, the memory and the touch screen. `cpuPerformance` follows the processor's model and its cores |
| `navigator.connection`'s `effectiveType`, `rtt` and `downlink` | The network's estimate. `rtt` and `downlink` also carry the renderer's noise, which changes with each renderer, so they change from run to run too |
| `jsHeapSizeLimit` of `console.memory` and `performance.memory` | V8's heap limit, which follows the memory. `performance.memory` gives it precise or rounded, as its cache holds it |
| `platformVersion` of `getHighEntropyValues()` | Windows' version |
| The keywords that match in `matchMedia()` for `orientation`, `pointer`, `any-pointer`, `hover`, `any-hover`, `color-gamut`, `dynamic-range`, `prefers-color-scheme`, `prefers-contrast`, `prefers-reduced-motion`, `prefers-reduced-transparency`, `forced-colors`, `inverted-colors`, `video-dynamic-range` and `display-state`, and the integers that match for `color` | The viewport's shape, the pointing devices, the display and the user's settings. Chrome 157 parses none of `inverted-colors`, `video-dynamic-range` and `display-state`, which then match nothing. Whether a Chrome Release parses one shows in the media text of each feature, which the Normal Form keeps |
| Whether `width`, `height`, `aspect-ratio`, `device-width`, `device-height` and `device-aspect-ratio` in `matchMedia()` agree with the window and the screen | The window and the screen give whole device-independent pixels, and a media query takes them as they are, which at some scales is not whole |
| `timeZone` of `Intl.DateTimeFormat`'s resolved options, `getTimezoneOffset()` over the year, and `Date`'s strings of a winter and a summer day | The time zone |

| Hidden as `<run>` | Why |
| --- | --- |
| `performance.timeOrigin`, each value of `performance.timing`, `document.timeline.currentTime` and `document.lastModified` | Times. `lastModified` is the time the page loaded, since its response has no `Last-Modified` |
| `key` and `id` of `navigation.currentEntry` | Random |
| `usedJSHeapSize` and `totalJSHeapSize` of `performance.memory` | Precise in a renderer that is locked to a site, as each is on the desktop, at most every 50 ms, from a cache that `console.memory`'s rounded figures share (`WindowPerformance::memory()` and `HeapSizeCache` in Blink's `core/timing`). So what they give depends on how long the walk took after it read `console.memory` ([#210](https://github.com/darionxt/mitto/issues/210)) |

The page's origin, name and port go behind `<origin>`, `<authority>` and `<host>` in every string, as on the wire, and `<port>` where a value is the port alone, as `location.port` is, with a port that is its scheme's own left out as a page leaves it out: `location`, `origin`, `document.URL`, `documentURI`, `baseURI` and `domain`, and `navigation.currentEntry.url`. A Page of a Parity Check loads the same page from a server on another port, which these hide. `console.memory`'s rounded figures, `RegExp`'s legacy statics and the error messages stay: [five sessions](baselines.md#the-script-fingerprint) gave the same, and the probe uses no regular expression ([runtime.md](runtime.md#the-probe)).

A place that a record lacks stays as it is, and so does a getter that threw at a place, since how it failed is the Runtime's and not the machine's. So a Page that lacks a member, or whose member throws, has a Normal Form all the same, which a Parity Check compares, and the failure shows in it.

### The form of a hidden value of a Script Fingerprint

`SCRIPT_FORMS` in [`tools/capture/machine_forms.py`](../tools/capture/machine_forms.py) holds the form of each value that the Normal Form hides as `<machine>`, by its place in the record: what Chrome on Windows can give a page there, with the Persona's `en-US` locale, such as a whole number from 1 for a size, a scale that Windows offers for `devicePixelRatio`, `24` or `30` for `colorDepth`, a time zone of [`time_zones.json`](../src/mitto/time_zones.json) for `timeZone`, and `Date`'s strings as `en-US` writes them. [`tests/test_machine.py`](../tests/test_machine.py) holds each value that a page reads of a Machine to these forms, for drawn Machines and for values that no draw gives, and names the places whose value a Page works out by Chrome's own code from its start message, such as `Date`'s strings, which the Parity Checks compare with real Chrome's instead.

The same command as for the wire checks real Chrome's values, in each `script/*.raw.json` under each directory. It also looks for each value of the machine in the Normal Form beside the raw record, in every place, the hidden ones aside: each text of four characters or more that is no keyword of Chrome's, such as a time zone's name, each number of 100 or more, such as a size or a heap limit, among what the getters and the third step gave, where an interface's constants are not, and each list of three numbers or more, such as the offsets. It prints how many it looked for and where one showed, never which:

```text
recordings/a: pass, 600 values in 13 raw recordings, and 1 raw records of a Script Fingerprint
  ...
  script/page.raw.json: pass, 73 values
    pass  globalThis.innerWidth: a whole number of device-independent pixels from 1, 1 values
    ...
    pass  none of 23 values of the machine shows in script/page.json
```

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
- The Platform Runtime component. After 15 minutes, that Chrome had other components, but none by this name in `chrome://components` and no `PlatformRuntime` directory. A study turns it on in 1% of Chrome 157 on Windows, where only an installed Chrome registers it, and none of ours had it ([baselines.md](baselines.md#the-platform-runtime-component)). Where it is, the source says that its library can change these values. We have not seen that.
- HTTP/3 at Google's own servers. The real sessions went over HTTP/2, so HTTP/3 comes from the capture server alone.
- Why Chrome 155 sends no `x-sec-ch-ua-not-a-border`. Its source has the same code, and it comes with a library of that name.
- Whether Google does anything with these lines, or answers differently without them. This page says what Chrome sends.
- `x-browser-year` and `x-browser-copyright` in another year. All three builds say 2026, so a change of year has not been seen, and the source says it follows the build.
- Other lines in the source that only apply with a policy, on Android or in a feature that a new directory does not use, such as `X-Goog-*` in Chrome's own requests, `YouTube-Restrict` and `X-CCT-Client-Data`. Lines that a page's own scripts set, such as `x-youtube-client-name`, are not Chrome's.

## Resumed connections and ALPS

**Baselines** hold fresh connections, because the capture server gives out no session tickets for them. A real server does, so a later connection can resume. Two questions were open about that, and about what a client sends once a server takes up its offer of ALPS.

| Question | Answer |
|---|---|
| Do Baselines need a resumed connection too? | No. In Chrome 155, 156 and 157 and in a **Session**, a resumed ClientHello is the fresh one with `pre_shared_key` added, and over HTTP/3 `early_data`. Everything else in the normal form stays the same, and both send the same things as early data. |
| Is checking the ClientHello enough for ALPS over HTTP/3? | Yes. What Chrome and a Session send in ALPS is empty, and the code has it as a constant. Settings from a server change nothing in the normal form, and what the `ACCEPT_CH` frame changes, a Session changes too. |

So neither calls for a change to what a Session sends or to the Baselines. A resumed connection adds nothing that a **Persona** decides. `tests/test_resumption.py` holds a Session to this, and with `MITTO_CHROME` set, `tests/capture/test_chrome.py` holds real Chrome to it.

### What Chrome and a Session do

This part comes from reading Chromium 157.0.8103.27. All of it is in `//net`, BoringSSL or QUICHE, so the two share it.

| What | Where | What it does |
|---|---|---|
| Keeping tickets | `SSLClientSessionCache` in `HttpNetworkSession`, `QuicClientSessionCache` in `QuicSessionPool` | In memory, so a new start has none. They go by name, port and proxy, and by top-level site where connections are [partitioned](parity.md#partitioning): `GetSessionCacheKey()`, which adds privacy mode over TCP, and `QuicCryptoClientConfigKey`. |
| Using a ticket | BoringSSL | Adds `pre_shared_key`, which has to be the last extension. A TLS 1.3 ticket is good for one connection. |
| Early data over TCP | `HttpNetworkSessionParams::enable_early_data` | Follows `EnableTLS13EarlyData`, which is off by default. [The native core](../native/feature_list.cc) turns it off as well. |
| Early data over QUIC | `TlsClientHandshaker`, `disable_tls_zero_rtt` | On, if the ticket allows it. |
| Which requests go early | `HttpNetworkTransaction`, `QuicHttpStream::DoRequestStream()` | Those with a safe method, or marked idempotent. Others wait until the handshake is confirmed. |
| ALPS over QUIC | `TlsClientHandshaker::SetAlpn()` | `SSL_add_application_settings()` with no data, for the ALPN of each IETF version. No setting reaches it. `use_new_alps_codepoint` only chooses the extension's number. |
| ALPS over TCP | `HttpNetworkSession` | `application_settings_[kProtoHTTP2] = {}` |
| Sending those settings | BoringSSL's `do_send_end_of_early_data()` | In an `EncryptedExtensions` message before the client's Finished, unless the server accepted early data. Then the settings of the session before carry over, and the server must not say any. |

A study could change the first of these. A search of the decompressed seeds of 2026-11-07, for all three Chrome Releases, finds no `EarlyData`, `TLS13`, `ZeroRtt`, `zero_rtt`, `0RTT`, `Alps`, `ALPS` or `disable_tls`, while `TryQuicByDefault` is there. If one drew `EnableTLS13EarlyData`, `chrome.txt` would name it.

### How the server does it

With `--tickets`, connections over TLS get tickets. Those over HTTP/3 allow early data. Those over TCP do not, because Python's `ssl` has no way to say so. Datagrams wait 100 ms before the server reads them, as if it were far away: on loopback a handshake can end before a client has written its request, and then nothing it would send early shows.

Three runs take a client through these requests, over each protocol. Each page asks for an answer that ends its connection before the next step: `Connection: close` over HTTP/1.1, and `GOAWAY` after the answer over HTTP/2 and HTTP/3.

| Request | Its connection | File |
|---|---|---|
| A navigation to the first page | Brings the tickets | Raw recording only, because it can be either kind, as [below](#what-only-chrome-does) |
| `resumption_fetch`, a `fetch()` | New, resumed | `resumption/http1.json`, `http2.json`, `http3.json` |
| `resumption_post`, a `fetch()` with a string | New, resumed | The same |
| `resumption_navigation`, a navigation from a script | New, resumed | The same |
| `resumption_other_site_script`, a `<script>` from another site | New, fresh | `resumption/http1.fresh.json`, `http2.fresh.json`, `http3.fresh.json` |
| A navigation to that other site | New. The script's ticket is for another top-level site | Raw recording only |

[`record.ps1 -Tickets`](baselines.md) takes a Chrome through them. These runs are not Baselines, and none of them went into `tests/baselines`.

### What we measured

This part was measured on 2026-11-07. Chrome was stable Windows 155.0.8059.85, 156.0.8078.80 and 157.0.8103.39, windowed, from new user data directories with a variations seed and [the flags of a Baseline](baselines.md#what-it-does), three sessions of each. A Session was a macOS arm64 build of the native core, with each **Persona**, two sessions of each. All three sessions of a Chrome Release gave identical normal forms.

#### A resumed connection against a fresh one

The fresh connections have the same `tls`, `http2`, `http3` and `quic` as `tests/baselines/RELEASE/cases/*.json`, in all nine files. Against those:

| | Over HTTP/1.1 and HTTP/2 | Over HTTP/3 |
|---|---|---|
| What a resumed ClientHello adds | `pre_shared_key` | `pre_shared_key`, `early_data` |
| Every other extension, and the rest of `tls`, `http2`, `http3`, `quic` | The same | The same |

Some of what differs is not in a normal form, so we read it in the raw recordings of all 134 resumed connections of Chrome and all 54 of a Session. It was the same in both:

| | Over HTTP/1.1 and HTTP/2 | Over HTTP/3 |
|---|---|---|
| `pre_shared_key` | The last extension, with one identity, which is the server's ticket, and one binder | The same |
| `initial_round_trip_time` | | There on each resumed connection, and on no fresh one |
| First datagram | | 1250 bytes, as on a fresh connection |

The lines of each request were those of its kind in the Baselines, in the same order, apart from `Referer`, which names the page.

#### Early data

| What opens the connection | As early data over HTTP/3 | Over HTTP/1.1 and HTTP/2 |
|---|---|---|
| `fetch()` | The request, the control stream, the QPACK encoder stream | Nothing |
| A navigation | The request, the control stream, the QPACK encoder stream | Nothing |
| `fetch()` with a string, a `POST` | The control stream. The request waits for the end of the handshake | Nothing |

This is the same in each Chrome Release, each session and a Session.

To check that nothing goes early over TCP, the tickets have to allow it, so by hand we set `SSL_CTX_set_max_early_data()` through `ctypes` on the server's context:

| Client | `early_data` in a ClientHello with a ticket that allows it |
|---|---|
| Chrome 155, 156 and 157 | In none of 36 |
| The same, with `--enable-features=EnableTLS13EarlyData` | In 33 of 46, which shows that these tickets allow it |
| A Session, on Linux, because Python on macOS here has no such function to call | In none of 36 |

Most connections that offered it ended during the handshake against this server, which does not read early data, and we did not look into why. So what Chrome would send early over TCP is not known. With the feature off, that does not matter.

#### A Session against Chrome

For each Persona and protocol, the normal forms are those of that Chrome Release, in both files, apart from two things that have nothing to do with resuming:

- The navigation came from a script, so Chrome sends no `Sec-Fetch-User`, and a Session's navigation has `?1`.
- The page adds `resumption_other_site_script` from a script, so over HTTP/2 and HTTP/3 Chrome gives it a lower priority than a Session's `script`, with no `priority` line.

A navigation to the other site was fresh in all 27 runs of Chrome and all 18 of a Session, although the script had just got a ticket for that name. So a ticket stays with its top-level site in both.

#### What only Chrome does

Chrome opens connections that carry no request, and these take tickets as well. Over HTTP/1.1 and HTTP/2, one opened before the first navigation, so that navigation itself came over a resumed connection in 17 of 18 runs. Over HTTP/3 it was fresh in all 9. A Session opens no such connections, so its first request to a name always comes over a fresh one. No normal form holds this, as with [which requests share a connection](#what-it-does-not-capture-yet).

#### ALPS over HTTP/3

Neither Python's `ssl` nor aioquic can negotiate ALPS. aioquic's TLS is written in Python, though, so for this a subclass of it, not in the kit, puts `application_settings` in the server's `EncryptedExtensions`, and takes the client's `EncryptedExtensions` into the transcript before its Finished. That is how we read what a client sends.

| The server says | The client's `EncryptedExtensions` | The normal forms |
|---|---|---|
| Nothing | `08 000006 0004 44cd 0000`: one `application_settings` with nothing in it, in all 20 connections of Chrome and all 111 of a Session | `http3.json` and `cases/http3.json` of each Chrome Release are byte for byte the Baselines. A Session has no gaps from the 87 HTTP/3 Baselines of those runs |
| `SETTINGS` | The same, in all 19 and all 111 | The same |
| `SETTINGS` and `ACCEPT_CH` for `sec-ch-ua-arch` | The same, in all 6 and all 3 | Only the navigation differs from the Baselines. It has `sec-ch-ua-arch`, between `accept` and `sec-fetch-site`, on the request that went out, and a Session's normal form is the same as Chrome's throughout |
| `SETTINGS`, with tickets | The same on the 9 and 18 connections with no early data accepted, and no message on the 9 and 18 that had it | The same as the resumption runs without ALPS |

Google's real servers negotiate it too, as [the next part](#at-real-servers) shows.

#### At real servers

The capture server is ours, so we also went to real servers, on 2026-11-07. This ran on a hosted Linux runner, with stable Chrome 156.0.8078.79 and a Session, over real UDP. Each client asked for a page, waited 45 seconds until its connection had ended, and asked again. `tcpdump` gave the Initial packets, which anyone can read, and a net log gave what Chrome got. A script read the net logs and printed names only.

| Server | Client | Later connections against the first | 0-RTT packets | Token in the Initial packet |
|---|---|---|---|---|
| `www.google.com` | Chrome | `pre_shared_key` and `early_data` are the only extensions that differ, on all 3 | 1 to 4 | 70 to 86 bytes, and none on the first |
| `www.google.com` | A Session | The same, on the 1 it opened | 3 | 70 bytes, and none on the first |
| `www.cloudflare.com` | Chrome | `pre_shared_key` is the only one that differs, on all 4 | None | None |

The net logs fit that. At `www.google.com`, every session has `QUIC_ACCEPT_CH_FRAME_RECEIVED`, which only comes from ALPS, so that server does negotiate it over HTTP/3. Later ones have `QUIC_SESSION_ZERO_RTT_STATE` with `AttemptedAndSucceeded`. At `www.cloudflare.com` they have no such frame and `NotAttempted`, so its tickets do not allow early data, and neither client offers it unless they do.

The token is what a server's `NEW_TOKEN` frame leaves for next time. The capture server sends none, so it never shows there, and both clients sent one here. A Session has no net log, so it says nothing of what `www.google.com` said in ALPS beyond answering with a 200 over HTTP/3. We compared a Session at `www.google.com` only.

### What this does not cover

- What [What it does not capture yet](#what-it-does-not-capture-yet) lists under ALPS and resumed connections.
- The order and size of what goes in 0-RTT packets, because aioquic hands over stream data only. Only the streams are compared.
- A Session on Windows, which is not a **Host**, and Chrome on a second machine.
- A ticket that has run out, or that a server has forgotten, and how each client falls back.
- A later Chrome Release. When the Engine Release moves, `record.ps1 -Tickets` repeats the first part. For ALPS, `TlsClientHandshaker::SetAlpn()` is the place to read: while it passes no data, there is nothing to see.

## Real ECH

A **Baseline**'s ClientHello holds ECH GREASE, because Chrome finds no HTTPS record for the names of the case pages. A site whose HTTPS record offers ECH gets real ECH instead: an outer ClientHello at the record's public name, which carries the real one, the ClientHelloInner, encrypted to the site's key. So we recorded Chrome against HTTPS records of our own, on 2026-11-07:

| Question | Answer | From |
|---|---|---|
| Can Chrome use a resolver of our own, without a change to the machine's DNS settings? | Yes. `dns_over_https` in the `Local State` of its user data directory sends every name to a DoH server on loopback | Measured |
| Does that change what Chrome sends? | No. With HTTPS records that offer no ECH, the three runs of the case pages over TLS came out byte for byte the Baselines | Measured |
| Does Chrome then send real ECH? | Yes, to every name whose record offers it, over TCP and over QUIC | Measured |
| Can the ClientHelloInner be read? | Yes, with the private key of the configuration in the record. Once it is decrypted, the kit's parsers read it as they read the outer one | Measured |
| Do Chrome and a **Session** send the same? | Yes. The outer ClientHello has the normal form of the Baselines, which hold GREASE, and the ClientHelloInner is the same in Chrome 155, 156 and 157 and in a Session | Measured |
| What is in the ClientHelloInner? | The outer ClientHello's offer for TLS 1.3 alone, at the real name | Measured, and the source |
| Can a Baseline hold this? | Yes, with a DoH server and a key in the kit, and over HTTP/3 a server that accepts ECH | Measured |
| Is it worth a **Recording Case**? | No. `tests/test_dns.py` holds a Session to what Chrome sent | [Below](#why-no-recording-case) |

### What Chrome does

This part comes from reading Chromium 157's source.

`StubResolverConfigReader` in `chrome/browser/net/` reads `dns_over_https.mode` and `dns_over_https.templates` from `Local State`. With the mode `secure`, every name goes to the servers of the templates and nowhere else. It turns DoH off on a machine that is joined to a domain or has machine-level policies, and when Windows parental controls filter the web. A template without `{?dns}` makes a `POST`, which `DnsHTTPAttempt` sends around any proxy.

`SSLConnectJob` and `TlsStreamAttempt` give BoringSSL the ECHConfigList of the HTTPS record of the endpoint they connect to, when `GetEchMode()` allows ECH, which it does in Chrome and in a Session without a policy. A server that cannot decrypt the ClientHelloInner completes the handshake for the public name. BoringSSL checks the certificate against the public name, and the handshake fails with `ERR_ECH_NOT_NEGOTIATED`. `//net` then connects again with the `retry_configs` the server sent, or with none, which brings back ECH GREASE. Over QUIC it does not connect again without ECH.

BoringSSL writes both ClientHellos in `ssl_add_clienthello_tlsext()` and encrypts the inner one in `ssl_encrypt_client_hello()`. The inner one leaves out what only TLS 1.2 uses, puts what it shares with the outer one in `ech_outer_extensions`, and is padded to a multiple of 32 bytes. GREASE gets a payload of 128, 160, 192 or 224 random bytes, and the AEAD's 16. Both pick AES-128-GCM on a processor with AES instructions and ChaCha20-Poly1305 on one without, so that choice follows the **Host**, for real ECH among what the record offers.

After the handshake, `SSLInfo::encrypted_client_hello` says whether the server accepted ECH. DevTools, metrics and the HTTP cache read it, and nothing that changes what a server sees.

### How we measured it

| Part | How |
|---|---|
| Chrome | Stable Windows Chrome 155.0.8059.85, 156.0.8078.80 and 157.0.8103.39, windowed, from new user data directories. A copy of `record.ps1` took each through the runs of the case pages over HTTP/1.1, HTTP/2 and HTTP/3, with the flags of a Baseline apart from `--host-resolver-rules`, and started the DoH server too. `--ignore-certificate-errors-spki-list` covered that server as well, which presented the capture server's certificate. The copy took the seed's serial number in `Local State` as the sign that the seed had come, with or without `variations_country`, and `chrome://version` listed active variations in every run |
| Its resolver | After the first start had brought the variations seed and Chrome had closed, a script wrote `"dns_over_https": {"mode": "secure", "templates": "https://127.0.0.1:PORT/dns-query"}` into `Local State`. That server answers no other name, so the seed had to come first |
| The DoH server | A script beside the capture server, over HTTP/1.1. It answers the names of the case pages with loopback and with an HTTPS record for each port, such as `_8444._https.capture.test` with `alpn=h2`. Any other name does not exist, so Chrome's own requests got no address and nothing left the machine |
| The ECH configuration | A key of its own for each session, with X25519, HKDF-SHA256 and AES-128-GCM, the public name `public.test` and a `maximum_name_length` of 0. The outer ClientHello holds the public name, which the normal form writes as `<host>` |
| Three kinds of session | Records with no ECH. Records with ECH, which the capture server refuses, because Python's `ssl` knows no ECH. Records with ECH, which a server patched by hand accepts |
| A Session | macOS arm64, with the Persona of each Chrome Release, through a DNS server on loopback with the same records and a key of its own. Every case of the same runs, replayed as a **Parity Check** replays it |
| Reading the ClientHelloInner | HPKE of RFC 9180 in Python, in `tests/local_ech.py` |

The accepting server leaned on private parts of aioquic, so it stays out of the kit. aioquic's TLS took the ClientHelloInner in the place of the outer one and put the confirmation of RFC 9849, section 7.2, at the end of its ServerHello's random, and over TCP it ran behind a TLS 1.3 record layer written for this. The kit still recorded the outer ClientHello.

### What we measured

Nine sessions: one with no ECH, from Chrome 157, and one with ECH refused and one with it accepted from each Chrome Release, with a second of each from Chrome 157. Next to the Baselines of its Chrome Release, each sent this:

| Session | HTTP/1.1 and HTTP/2 | HTTP/3 |
|---|---|---|
| No ECH | Byte for byte the Baselines | Byte for byte the Baselines |
| ECH refused | Each connection sends real ECH, fails, and connects again with GREASE. Only the second connection carries requests, so the normal forms are byte for byte the Baselines | Six or seven connections with real ECH and no request. The page never loads |
| ECH accepted | Byte for byte the Baselines | Byte for byte the Baselines |

So with real ECH the outer ClientHello has the normal form of the Baselines, and with ECH accepted, so does everything after it. The ClientHelloInner differs from the outer one like this:

| Part | Over TCP | Over QUIC |
|---|---|---|
| `encrypted_client_hello` | `inner`, with no KDF and AEAD | The same |
| Cipher suites | The three for TLS 1.3, with GREASE | Those of the outer one |
| `supported_versions` | TLS 1.3, with GREASE | Those of the outer one |
| `ec_point_formats`, `extended_master_secret`, `renegotiation_info`, `session_ticket` | Left out | In neither |
| `server_name` | The real name | The real name |
| The rest | Those of the outer one, in `ech_outer_extensions` | The same, with `supported_versions` there too |

The eight sessions with ECH sent 196 ClientHellos with real ECH. Over TCP they had one outer and one inner normal form, and over QUIC one of each, in all three Chrome Releases. Each had an encapsulated key of 32 bytes and a payload of 144, which is 128 bytes of padded ClientHelloInner and the AEAD's 16, for each of the three names. A Session with the Persona of each Chrome Release sent the same normal forms, the same list in `ech_outer_extensions` and the same sizes, and every case of the same runs matched Chrome's, as a Parity Check compares them. Over HTTP/3, `quic` and the transport parameters were those of the Baselines. Over QUIC with ECH refused, a Session's request failed with `ERR_QUIC_PROTOCOL_ERROR`.

### Why no Recording Case

A Recording Case would add little that is not compared already:

- What the network and a server without the key see is the outer ClientHello, and its normal form is that of every Baseline. BoringSSL makes GREASE look like real ECH on purpose. What differs is the configuration ID, the encapsulated key and the payload, which are random or come from the record, and which the normal form drops.
- What comes after the handshake does not depend on ECH, by the source, and it was byte for byte the Baselines with ECH accepted.
- The ClientHelloInner comes from the same BoringSSL and the same settings as the outer one, by a rule that the table above shows. `tests/test_dns.py` holds a Session to that rule over TCP and QUIC, so when a new **Engine Release** changes the rule, a test fails.

It would cost a DoH server and an ECH key in the kit, a step in `record.ps1` that writes `Local State` before the runs, a place for the ClientHelloInner in the normal form, and a Parity Check whose Session finds names through a DNS server. Over HTTP/3 it would also need a server that accepts ECH, which neither Python's `ssl` nor aioquic offers.

### What this does not cover

- A Chrome Release that changes the ClientHelloInner and keeps the outer one. The test only shows what the Engine Release does, so seeing that means recording Chrome again, as here.
- `retry_configs`. The capture server cannot send them, so connecting again with them comes from the source. A Session has the same code.
- Another configuration: other cipher suites, several configurations, another `maximum_name_length`, or a public name that the certificate does not cover.
- A HelloRetryRequest with ECH, and a ticket with ECH.
- A real server's ECH. No real site was compared this time.
- QUIC that is not forced on. A refused ECH over QUIC leaves a page with TCP alone, and Chrome's choice between the two then was not measured.
- A record that comes late. `//net` waits a short time for the HTTPS answer after the address answer, and then goes on with GREASE.

## From Python

A Parity Check can run the server itself. It picks free ports unless told otherwise, and it listens as long as the `with` block runs. The routes into it are `HTTP1`, `HTTP2`, `HTTP3`, `PLAIN_HTTP`, `PROXIED_HTTP1`, `PROXIED_HTTP2` and `SCRIPT`, the last for the Script Fingerprint's pages, each with a recording of its own:

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

`SCRIPT_RUNS` in `tools/capture/script_page.py` lists the runs of a Script Fingerprint, and `server.start_url(run)` and `server.chrome_flags(run)` take them too. `script_normal_form(record, url)` gives the Normal Form of a record of the probe that ran in a page at `url`, or with `None` for a page with no origin of its own, such as `about:blank`, and `dumps_record()` writes it as a Baseline.

`CaptureServer(tickets=True)` gives out tickets, and `RESUMPTION_RUNS` lists the runs that use them. `normal_form(recording, run.cases)` gives the resumed connections of such a run, and `normal_form(recording, run.fresh_cases)` the fresh ones. They cannot go in one normal form, because a normal form holds what its connections have in common.

To reach a case page, a client needs `server.host_resolver_rules`, which a native Session takes as `host_resolver_rules` from `mitto._native`. `CASE_HOST` names the page, and `server.url(route, host=CASE_HOST, path=...)` and `server.recording(route, host=CASE_HOST)` go with it, so that the placeholders are the ones the Baselines have. `RECORDING_RUNS` lists every run of the wire in the order a recording session takes them, `RUNS` those whose cases the Parity Checks replay, and `DOCUMENT_RUNS` [the others](#documents). `server.start_url(run)` and `server.chrome_flags(run)` say how to start one, and `normal_form(recording, run.cases, run.ordered)` gives a run's normal form with its `order`.


## Tests

`uv run pytest tests/capture` runs the kit's tests, including its fuzz targets on a fixed set of inputs. Files under `tests/capture/data` hold a real recording of Chrome, which the tests replay.

To run the same check against a real Chrome, headless, from fresh user data directories, name its executable in `MITTO_CHROME`. It goes through every run twice and checks that each **Recording Case** is what it says it is, and that the page of a Script Fingerprint gives the same Normal Form twice, with no debugging port. Without that, the test is skipped:

```bash
MITTO_CHROME="/path/to/chrome" uv run pytest tests/capture/test_chrome.py
```

Every parser we write has a fuzz target in `tests/capture/test_fuzz.py`. They take arbitrary bytes, built mostly from mutated real traffic and well-formed structures with random content, and may only fail in the way the parser documents. To look for new failures, with new inputs and many more of them:

```bash
uv run pytest tests/capture/test_fuzz.py --hypothesis-profile=fuzz
```
