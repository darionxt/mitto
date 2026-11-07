# Persona data

A **Persona** is the browser identity mitto presents to servers. Its data says what one **Chrome Release** on one operating system sends by default for each **Request Kind**. It is package data, so changing it needs no native rebuild. Terms in **bold** are defined in [CONTEXT.md](../CONTEXT.md).

There is one Persona for each Chrome Release in the **Supported Window**, all on Windows: `chrome_155_windows`, `chrome_156_windows` and `chrome_157_windows`. A **Session** takes one when it is created, and `mitto.Session()` and `mitto.get()` present the **Engine Release**'s. A request takes its Request Kind as `kind`, which is navigation for `GET` and fetch for other methods by default:

```python
with mitto.Session(persona="chrome_155_windows") as session:
    session.get(url)
    session.get(url, kind="script")
    session.post(url, json={"a": 1})
    session.post(url, kind="navigation", data={"a": "b"}, files={"file": ("a.txt", b"text", "text/plain")})
    session.get(url, kind="script", initiator="https://example.com/page")
```

## The file

Each Persona is one file, `src/mitto/personas/NAME.json`, which [`src/mitto/_persona.py`](../src/mitto/_persona.py) reads:

| Key | Meaning |
| --- | --- |
| `release` | The Chrome Release |
| `operating_system` | The operating system the Persona claims, independent of the **Host** |
| `accept_language` | The value for `Accept-Language`, which is a Session setting |
| `user_agent` | The value for `User-Agent` where no request's headers reach: a `CONNECT` and a fetch for a certificate. `//net` takes it from the Session's context. It is also the `User-Agent` line of each kind |
| `kinds.KIND.credentials_mode` | `same-origin`, as a page's `fetch()` has unless it asks otherwise, or `include`, as an element has |
| `kinds.KIND.priority` | The request's priority, such as `HIGHEST`, as in the C API's `MITTO_PRIORITY_HIGHEST` |
| `kinds.KIND.priority_incremental` | Whether the response can be used as it arrives |
| `kinds.KIND.headers` | `name: value` lines in the order Chrome sends them, with the case HTTP/1.1 sends |
| `kinds.KIND.body.lines` | The names in `headers` that only a request with a body has. A kind without `body` cannot have one, so a body with `script` or `image` is refused |
| `kinds.KIND.body.content_types` | The value for `Content-Type`, by what the body is: `text`, `form`, `json` or `multipart`. `<boundary>` stands for the boundary of a multipart body |

In `headers`, `<content-type>` stands for what the body says of itself or what a caller gives. `<origin>` is never sent, because the native core writes that line's value or removes the line. `Sec-Fetch-Mode` and `Sec-Fetch-Dest` say what the request is for, which the native core also needs on a later hop, and `Sec-Fetch-User` says that the user started it. `headers` also holds each [client hint](#client-hints) that a server can ask for, which the native core removes until one has.

## What comes from where

The file does not hold everything on the wire, because Chrome does not decide everything in one place either. mitto hands each part to `//net` the way Chrome's network service does, so that it lands where Chrome's does.

| On the wire | Comes from |
| --- | --- |
| `Host`, `Connection`, and the pseudo-headers of HTTP/2 and HTTP/3 | `//net` |
| The lines in `headers` | The C API's `headers`. HTTP/2 and HTTP/3 send the names in lower case |
| The values of `Origin` and `Sec-Fetch-*`, and whether those and `sec-ch-*` go at all | The native core, from the page a request comes from ([below](#the-initiator)) and what its origin [has asked for](#client-hints). A line in `headers` keeps its place, and one that the native core adds goes after the others |
| `Content-Length` | `//net`, after `Connection` or the pseudo-headers, from the C API's `body`. It says 0 for `POST` and `PUT` with no body at all, and for any method with an empty one |
| `Referer` | The native core, from the page a request comes from, or the C API's `referrer` for a caller's. `//net` drops a `Referer` among the headers, and sends this one after them |
| `Accept-Encoding` | `//net`, after `Referer`. The native core turns on Brotli and Zstandard as Chrome does, and `//net` advertises them only over HTTPS and to localhost |
| `Accept-Language` | `//net`, after `Accept-Encoding`, from the Session setting |
| `Cookie` | `//net`, after `Accept-Language`, from the Session's cookie store ([cookies](parity.md#cookies)) |
| The `priority` header, last in HTTP/2 and HTTP/3 and absent in HTTP/1.1 | `//net`, from `priority` and `priority_incremental` |
| The HTTP/2 stream weight and the HTTP/3 `PRIORITY_UPDATE` | `//net`, from the same two values |

So a line for `Referer`, `Accept-Encoding`, `Accept-Language` or `priority` does not belong in `headers`. Giving `Referer` there is an error, and giving the others would put them in the wrong place, or send `priority` over HTTP/1.1 as well.

## What a body adds

`data`, `json` and `files` make a body, and the native core hands it to `//net`, which adds `Content-Length`. `Content-Type` goes out with a body on a fetch or a navigation, and `Cache-Control: max-age=0` on a navigation only. Each has its place in `headers` in the file, from the Baselines. What the body is decides its `Content-Type`:

| Argument | The body | `Content-Type` of a fetch | `Content-Type` of a navigation |
| --- | --- | --- | --- |
| `data=b"..."` | As it is | None, as for an `ArrayBuffer` | None |
| `data="..."` | UTF-8 | `text/plain;charset=UTF-8` | `text/plain` |
| `data={...}` | As `URLSearchParams` writes it | `application/x-www-form-urlencoded;charset=UTF-8` | `application/x-www-form-urlencoded` |
| `json=...` | As `JSON.stringify` writes it, except that a float keeps Python's text. NaN and the infinities are refused, where `JSON.stringify` would write `null` | `application/json` | `application/json` |
| `files={...}`, with text fields in a mapping in `data` | As Blink's `FormData` writes it, with text fields first | `multipart/form-data; boundary=...` | The same |
| `data=file`, a file object in binary mode | From where it stands, and with a `Content-Length` if it can seek. See [Streams](parity.md#streams) | None, as for a `File` with no type | None |
| `data=parts`, an iterable of bytes | Each part as it comes, and with no `Content-Length` unless `headers` gives one. See [Streams](parity.md#streams) | None, as for a `ReadableStream` | None |

A mapping follows `URLSearchParams` on a navigation too. A real form writes a line break in a value as `%0D%0A`, where `URLSearchParams` keeps a lone `%0A`, so a caller who wants the form's writes `\r\n`.

A multipart body has a boundary of `----WebKitFormBoundary` and 16 random letters and digits, drawn as Blink draws them. A field's name has `%0D%0A` for any line break and `%22` for a quote, and a file name has `%0D`, `%0A` and `%22`, so that neither can break out of its line. A value of a text field has its line breaks changed to CRLF, and a file's content stays as it is. A file with no name is Blink's `blob`. A file with no type, or one that holds a control character or anything else outside ASCII, says `application/octet-stream`, and any other type goes in lower case, as a `Blob` writes it. mitto does not guess a type from a file's name: what Chrome does depends on the operating system's own list, so it would differ between **Hosts**.

Fetch refuses a body with `GET` or `HEAD`, so a Session does too.

## The Initiator

`initiator=` takes the URL of the page a request is made from, which counts as a top-level page. Without it, a navigation counts as started by the browser, and a subresource as coming from the page at the root of the origin of its URL. A URL that is not valid ends the request with `ERR_INVALID_ARGUMENT`.

From the page and the URL of each hop, the native core sets these, in code ported from Chrome ([which code](parity.md#the-initiator-and-each-hop)):

| On the wire | What decides it |
| --- | --- |
| `Sec-Fetch-Site` | How far from the page's origin the URL and every URL before it have been: `same-origin`, `same-site` or `cross-site`. With no page it is `none` on every hop |
| `Referer` | `strict-origin-when-cross-origin`: the whole page on its own origin, its origin elsewhere, and nothing from HTTPS to HTTP. Once it has shrunk it stays that way, also when a redirect comes back |
| `Origin` on a fetch | The page's origin, for another origin or for a method but `GET` and `HEAD`. Once a redirect has left an origin other than the page's for yet another, it is `null` for the rest of the chain |
| `Origin` on a navigation | The page's origin for a method but `GET` and `HEAD`, or `null` with no page or from HTTPS to HTTP. `//net` makes it `null` at a redirect to another origin |
| `Origin` on a script or image | Nothing for `GET` and `HEAD` |
| `Sec-Fetch-Storage-Access` | `active` with `credentials_mode` of `include`, for a URL that is not of the same site as the page. So a cross-site `script` or `image` sends it, and a `fetch()` does not. A navigation becomes the first party, so it does not either |
| `sec-ch-*` and `Sec-Fetch-*` | Only for a URL that is potentially trustworthy, so not over plain HTTP to a host that is not local. A redirect to such a URL takes them away. A redirect from one adds `Sec-Fetch-*` on any kind, and `sec-ch-*` only on a navigation |
| The place of `sec-ch-*` | A navigation sends them again on each hop, after `Sec-Fetch-Dest`. A subresource keeps them where they are |

`//net` decides from the same page which cookies go ([cookies](parity.md#which-site-a-request-belongs-to)) and which connections a request can use ([partitioning](parity.md#partitioning)).

## Client hints

Chrome sends `sec-ch-ua`, `sec-ch-ua-mobile` and `sec-ch-ua-platform` to everyone. The other hints go once a server has asked, so a server that asks and gets nothing could tell. A **Session** keeps what each origin has asked for, for as long as the Session lasts. Which code this follows is [in parity.md](parity.md#client-hints).

| What a server says | What it does |
| --- | --- |
| `Accept-CH` on the response to a navigation | Replaces what the origin of that response has asked for. A redirect on the way counts, at once, for its own origin, and a 204 or 205 does not count, because Chrome shows nothing for it. An empty value takes everything back |
| `Accept-CH` on the response to a script, an image or a fetch | Nothing |
| `Accept-CH` that is not a list of tokens, or over plain HTTP to a host that is not local | Nothing. A token that names no hint counts for nothing, and names go in any case |
| A hint in both `Accept-CH` and `Critical-CH` that the navigation did not send | A navigation that started with `GET` or another safe method starts over from its first URL, with the hint, and the caller sees only the end of it. It does this once for each origin. A `POST` goes on, and the next navigation has the hint |
| `Clear-Site-Data` with `"clientHints"`, `"cookies"`, `"cache"` or `"*"` | Takes everything back for that origin, unless a subresource says it from another site, and `Accept-CH` and `Critical-CH` beside it count for nothing |
| The `ACCEPT_CH` frame in ALPS, for a navigation | The navigation sends those hints too, and nothing has gone out without them. They count for that request alone, as they do in Chrome, so what a server wants kept it says in `Accept-CH` |

| Request | Which of those hints it sends |
| --- | --- |
| A navigation | What the origin of its URL has asked for, on each hop. So `initiator=` plays no part |
| A script, image or fetch to its page's origin | What that origin has asked for, at its start |
| A script, image or fetch to another origin | None, because a page's permissions policy keeps them for its own origin and a Session has no other policy to go by |
| One of those on a redirect | Keeps what it has within its page's origin. Elsewhere it loses them, and does not get them back if it returns |

Over plain HTTP to a host that is not local, a request sends no client hints at all.

### Where they go

A navigation sends them in the order of `headers`, and on a later hop in that same order after `Sec-Fetch-Dest`. A restart for `Critical-CH` sends them after `Accept` and before `Sec-Fetch-Site`. The hop right after it sends those again first, and then what is new, [as Chrome does](parity.md#starting-over). One for the frame in ALPS leaves the lines it had in place, and adds the new ones there.

A script, image or fetch has no order of its own to write down. Blink holds those lines in a hash table, so their order depends on which lines there are, and the same two hints can swap places when a server asks for a third. In `headers`, a subresource therefore has its lines in the order they go with no hint added, then the hints. Once a hint goes, the native core [works out the order](parity.md#the-order-of-a-subresources-lines) of `User-Agent`, `Content-Type` and the hints, ahead of the other lines, as Blink's table gives it.

### What they say

| Hints | The Persona says | Because |
| --- | --- | --- |
| `sec-ch-ua-full-version`, `sec-ch-ua-full-version-list` | The build that recorded the Baselines of the Chrome Release | The Baselines hold this |
| `sec-ch-ua-arch`, `sec-ch-ua-bitness`, `sec-ch-ua-wow64`, `sec-ch-ua-model`, `sec-ch-ua-form-factors` | `"x86"`, `"64"`, `?0`, `""`, `"Desktop"` | The Baselines hold this, and it is the same for a 64-bit Chrome on any x64 desktop PC with Windows |
| `sec-ch-ua-platform-version` | `"19.0.0"` | The Baselines hold this. Chrome reads it from Windows, so it follows the machine too. It only says how new Windows is, and Windows 11 24H2 says the same |
| `device-memory`, `sec-ch-device-memory` | `8` | A value [no Baseline holds](capture.md#what-stays-and-what-does-not). A common size, where the real value is the machine's, rounded |
| `dpr`, `sec-ch-dpr` | `1` | No Baseline holds it. The scale of a display at 100% |
| `viewport-width`, `sec-ch-viewport-width`, `sec-ch-viewport-height` | `1920`, `1920`, `945` | No Baseline holds it. A Chrome window that fills a 1920 by 1080 display at that scale |
| `rtt`, `downlink`, `ect` | `50`, `10`, `4g` | No Baseline holds it. A good connection. In Chrome they follow what it measures, with some noise for each host |
| `sec-ch-prefers-color-scheme`, `sec-ch-prefers-reduced-motion`, `sec-ch-prefers-reduced-transparency` | `light`, `no-preference`, `no-preference` | No Baseline holds it. What Windows has as it comes |

`width` and `sec-ch-width` say how wide an image is laid out, which only a page knows, and Chrome sends neither for an image with no width of its own. `save-data` needs a setting that a Persona does not have on. So a Persona has no line for them, and they do not go.

## What a caller changes

`Session(accept_language=...)` replaces the Persona's `accept_language`, and an empty string sends no `Accept-Language`. For one request, `headers` changes the Persona's lines, by name in any case:

| In `headers` | On the wire |
| --- | --- |
| A name the Persona has | Takes the place of the Persona's line, which keeps the Persona's spelling of the name, as HTTP/1.1 shows |
| Another name | After the Persona's lines, in the order given, before what `//net` adds |
| A name the Persona has, with `None` | The line is gone |
| Another name, with `None` | Nothing changes, so this cannot remove what `//net` adds |
| `User-Agent`, with `None` | The line leaves its place, and `//net` adds the Persona's `user_agent` after the other lines, because a request always gets one from the context |
| `Referer` | Goes as given to every hop or, with `None`, goes from every hop, in place of what the Initiator would make of it. `//net` still decides where it goes |
| A line that the native core sets, such as `Origin`, `Sec-Fetch-Site` or `sec-ch-ua` | Stays as given or, with `None`, stays gone on each hop, also over plain HTTP. `//net` still changes an `Origin` on a redirect, as it does in Chrome: `null` for another origin, and gone when the method changes |
| A client hint that no server has asked for | Goes as given, on each hop |
| `Content-Type`, with a body | Takes the place of the line in the file, whatever the body is, so a type for bytes goes where a text body's would. `None` removes it |

On a script, image or fetch that sends a hint a server asked for, `User-Agent`, `Content-Type` and the hints go [in the order of Blink's table](#where-they-go), so a caller's own `Content-Type` goes there too, as a page's does.

A name that `//net` would add, such as `Accept-Encoding`, counts as another name: it follows the Persona's lines, and `//net` then leaves it alone. `Cookie` is the exception: it keeps that place, but `//net` writes over its value whenever the store holds a cookie for the URL, and it does not follow a redirect to another origin.

## Write one

Write the file from the Baselines of its Chrome Release in [`tests/baselines`](../tests/baselines), by taking away what the table above leaves to `//net`. Over the three protocols, a Request Kind differs only in what `//net` adds and in the case of its names, so one list of lines serves all three. The lines of a body come from the cases `navigation_form_*`, `fetch_post_text_*` and `fetch_post_json_same_origin`, in `cases/`, and the client hints from `client_hints_form`, `client_hints_fetch_post`, `client_hints_script` and `client_hints_image`, with [the values above](#what-they-say) for what a Baseline does not hold.

[`tests/test_persona.py`](../tests/test_persona.py) compares the file with every Baseline of the main **Request Kinds** of its Chrome Release, including HTTP/3, and with the order and the values of those cases, without a native library. It writes out what `//net` adds. The [Parity Checks](parity.md) then show what `//net` and the native core really do. In the cases of a body, the value of `Sec-Fetch-Site` is left out there, because the native core sets it from the page.

## What differs between Chrome Releases

An older Chrome Release runs on the network code of the **Engine Release**, and differs only by its Persona ([ADR-0005](adr/0005-older-releases-as-data.md)). Chrome 155 and 156 need nothing else: [their Baselines](baselines.md#what-differs-from-chrome-157) differ from Chrome 157's in `sec-ch-ua` and `User-Agent` only, along with `sec-ch-ua-full-version` and `sec-ch-ua-full-version-list` where a server has asked, so those four lines are all that differs between the three files. The Baselines of [the Recording Cases](baselines.md#the-recording-cases) agree, with redirects, bodies, cookies, plain HTTP and a proxy: every case differs in those lines only. Plain HTTP and a `CONNECT` have no `sec-ch-ua`, so there it is `User-Agent` alone.

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

Baselines record a fresh connection that goes well. Everything else is the Engine Release's whichever Persona is presented, which is the imperfect emulation that [ADR-0005 accepts](adr/0005-older-releases-as-data.md#consequences). Between 155 and 157, it changed in these places:

| What | Where it shows |
| --- | --- |
| The HSTS preload list. Chrome 155 and 156 have the same one. 2,730 hosts are only in Chrome 157's and 2,361 only in theirs | A plain HTTP request to one of those hosts: the native core asks for HTTPS as Chrome 157 does, whichever Persona it presents |
| QUIC connection migration, in `net/quic/quic_chromium_client_session.cc` and `quic_migration_attempt_context.cc`, with new features and new defaults | When the network or the path changes |
| Closing an HTTP/2 session after a protocol error, in `net/spdy/spdy_session.cc` | When a server breaks the protocol |
| `HttpCacheZstdCompression` and `HttpCacheZstdDecompression` by default from 157, and `LogicalClearHttpCache` from 156 | What a [cache](parity.md#the-http-cache) keeps on disk, and never on the wire |
| A 206 response that runs past the range asked for fails with `ERR_CONTENT_LENGTH_MISMATCH` instead of going into the cache, in `net/http/partial_data.cc`, `http_cache_writers.cc` and `http_cache_transaction.cc`. 157.0.8103.39 and 156.0.8078.80 have it, and 155.0.8059.86 does not | A Session with a cache, when a server sends more than the range that a request asked for |
