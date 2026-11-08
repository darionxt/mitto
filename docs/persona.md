# Persona data

A **Persona** is the browser identity mitto presents to servers. Its data says what one **Chrome Release** on one operating system sends by default for each **Request Kind**. It is package data, so changing it needs no native rebuild. Terms in **bold** are defined in [CONTEXT.md](../CONTEXT.md).

There is one Persona for each Chrome Release in the **Supported Window**, all on Windows, named `chrome_RELEASE_windows`. [`persona=`](api.md#persona) lists them. A **Session** takes one when it is created, and `mitto.Session()` and `mitto.get()` present the **Engine Release**'s. A request takes its Request Kind as `kind`, which is navigation for `GET` and fetch for other methods by default:

```python
with mitto.Session() as session:
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

In `headers`, `<content-type>` stands for what the body says of itself or what a caller gives, and `<machine>` for the value that the Session's [Machine](#a-machine-of-its-own) gives a hint that follows the machine. `<origin>` is never sent, because the native core writes that line's value or removes the line. `Sec-Fetch-Mode` and `Sec-Fetch-Dest` say what the request is for, which the native core also needs on a later hop, and `Sec-Fetch-User` says that the user started it. `headers` also holds each [client hint](#client-hints) that a server can ask for, which the native core removes until one has.

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
| `device-memory`, `sec-ch-device-memory`, `dpr`, `sec-ch-dpr`, `viewport-width`, `sec-ch-viewport-width`, `sec-ch-viewport-height`, `rtt`, `downlink`, `ect`, `sec-ch-prefers-color-scheme`, `sec-ch-prefers-reduced-motion`, `sec-ch-prefers-reduced-transparency` and `sec-ch-ua-platform-version` | `<machine>`, which a Session fills from its **Machine** when it opens | Chrome reads them from the machine it runs on, and the Session claims [a machine of its own](#a-machine-of-its-own), which [`machine=`](api.md#machine) gives or which is drawn for it |

`rtt` and `downlink` go with [Chrome's noise for each host](#the-noise-on-rtt-and-downlink), its cap and its rounding, which the native core works out on each hop.

In mitto 0.1 every Session claimed one machine, which [`Machine.named("mitto-0.1")`](api.md#machinenamed) still gives:

| Hints | The Machine of mitto 0.1 says | Because |
| --- | --- | --- |
| `sec-ch-ua-platform-version` | `"19.0.0"` | The Windows that recorded the Baselines says this, which the normal form hides. It only says how new Windows is, and Windows 11 24H2 says the same |
| `device-memory`, `sec-ch-device-memory` | `8` | A value [no Baseline holds](capture.md#what-stays-and-what-does-not). A common size, where the real value is the machine's, rounded |
| `dpr`, `sec-ch-dpr` | `1` | No Baseline holds it. The scale of a display at 100% |
| `viewport-width`, `sec-ch-viewport-width`, `sec-ch-viewport-height` | `1920`, `1920`, `945` | No Baseline holds it. A Chrome window that fills a 1920 by 1080 display at that scale |
| `rtt`, `downlink`, `ect` | `50`, `10`, `4g` | No Baseline holds it. A good connection, of 50 ms and 10,000 kbps. In Chrome they follow what it measures, with some noise for each host, which the Machine now gets too, so its `downlink` is from `9` to `10` by host |
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

[`tools/release/persona.py`](../tools/release/persona.py) writes the file from the Baselines of its Chrome Release in [`tests/baselines`](../tests/baselines), with the Chrome Release in place of `RELEASE`:

```bash
uv run python -m tools.release persona RELEASE
```

It takes away what the table above leaves to `//net`. Over the three protocols, a Request Kind differs only in what `//net` adds and in the case of its names, so one list of lines serves all three. The lines of a body come from the cases `navigation_form_*`, `fetch_post_text_*` and `fetch_post_json_same_origin`, in `cases/`, and the client hints from `client_hints_form`, `client_hints_fetch_post`, `client_hints_script` and `client_hints_image`, with `<machine>` for [the hints that follow the machine](#what-they-say). The priority comes from the `priority` header over HTTP/2. The script also holds `credentials_mode` and the `Content-Type` of a body that no Recording Case sends, and refuses a Baseline whose `Content-Type` is not its own. [`tests/release`](../tests/release) checks that it writes each file in the repository byte for byte.

[`tests/test_persona.py`](../tests/test_persona.py) compares the file, with the values of the Machine of mitto 0.1, with every Baseline of the main **Request Kinds** of its Chrome Release, including HTTP/3, and with the order and the values of those cases, without a native library. It writes out what `//net` adds. The [Parity Checks](parity.md) then show what `//net` and the native core really do. In the cases of a body, the value of `Sec-Fetch-Site` is left out there, because the native core sets it from the page.

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

## A machine of its own

In mitto 0.1, once a server asked for the hints that follow the machine, every Session of a Persona sent [the same values](#what-they-say). Each is a value a real Chrome sends, but real Chrome users spread over many machines, so all Sessions landed on one point, which a server can count. A Session that saved its cookies and came back also came back as that one machine.

So each Session now claims a **Machine** of its own, drawn from [a table](#the-table) unless [`machine=`](api.md#machine) gives one. This section says what decides each of those values in Chrome, which values go together, how common each one is, and how the table and a draw are made. The decision is [ADR-0007](adr/0007-a-machine-of-its-own-per-session.md), and [M5](roadmap.md#m5-a-machine-for-each-session) builds it. Everything it plans is built: the Machine, its draw from the table, its values in the hints, the Machine saved with the cookies, the noise on `rtt` and `downlink` for each host, `sec-ch-ua-platform-version` hidden in the normal form, and a step of the release routine that refreshes the table.

No Baseline holds these values, and recording them on other machines would put those machines in the repository. So what follows comes from Chromium's source at 155.0.8059.85, 156.0.8078.80 and 157.0.8103.39. What each file named here does with these values is the same in all three, apart from checks that became `CHECK`, so each answer holds for every **Chrome Release** in the **Supported Window**. The one exception is in the table: Chrome 157 lets a contrast theme decide the colour scheme, and 155 and 156 do not. Each path is under `chromium/src`.

### What decides each value

| Hints | What Chrome reads | What it sends on Windows | Where |
| --- | --- | --- | --- |
| `device-memory`, `sec-ch-device-memory` | The memory that Windows reports, in MiB, taken to the nearer power of two, and to the lower one on a tie. A desktop then holds it between 2 and 32 GiB, and Android between 1 and 8 | `2`, `4`, `8`, `16` or `32`. `8` stands for more than 6 GiB and up to 12 GiB, so a machine with 16 GB sends `16` | `third_party/blink/common/device_memory/approximated_device_memory.cc` |
| `dpr`, `sec-ch-dpr` | The scale of a display, times the zoom of the page. A navigation takes the primary display's scale, and a script, image or fetch the scale of the display its window is on. On Windows a display's scale is its DPI over 96, which is the scale in Windows' display settings | `1`, `1.25`, `1.5`, `1.75`, `2` and so on, as the shortest decimal | `content/browser/client_hints/client_hints.cc` (`GetDeviceScaleFactor`, `GetZoomFactor`), `third_party/blink/renderer/core/loader/frame_fetch_context.cc`, `ui/display/win/dpi.cc` |
| `viewport-width`, `sec-ch-viewport-width`, `sec-ch-viewport-height` | The size of the window's page area in device-independent pixels, divided by the zoom and rounded. A navigation reads it from the tab's view, or else from the last top-level page that the profile laid out, and leaves the line out if it has neither. A script, image or fetch reads the page's layout size, which on a desktop is the whole page area, a scrollbar included, so both give the same value | Whole numbers. A window that fills the display has the display's work area, which leaves out the taskbar, less Chrome's own bars. With no saved size, Chrome's first window on a display of 16:10 or wider, with a work area wider than 1600, is half the work area less 15 wide. After that Chrome keeps the size the user left | `content/browser/client_hints/client_hints.cc` (`GetViewportSize`, `GetScaledViewportSize`), `third_party/blink/renderer/core/frame/local_frame_view.cc` (`ViewportWidth`, `ViewportHeight`), `chrome/browser/ui/window_sizer/window_sizer.cc` (`GetDefaultWindowBounds`) |
| `rtt` | The round trip of HTTP that Chrome's network quality estimator has measured, with [noise for each host](#the-noise-on-rtt-and-downlink), at most 3000 and rounded to 50 ms | A multiple of 50 from `0` to `3000` | `content/browser/client_hints/client_hints.cc` (`RoundRtt`), `third_party/blink/renderer/platform/network/network_state_notifier.cc` (`RoundRtt`) |
| `downlink` | The throughput the estimator has measured, with the same noise, at most 10,000 kbps and rounded to 50 kbps, in Mbps | A multiple of 0.05 from `0` to `10`, as the shortest decimal, such as `2.65` | `content/browser/client_hints/client_hints.cc` (`RoundKbpsToMbps`), `third_party/blink/renderer/platform/network/network_state_notifier.cc` (`RoundMbps`) |
| `ect` | The same round trip, with no noise: `slow-2g` from 2010 ms, `2g` from 1420 ms, `3g` from 272 ms, and `4g` below that or with no estimate at all | `4g`, `3g`, `2g` or `slow-2g` | `net/nqe/network_quality_estimator.cc`, `net/nqe/network_quality_estimator_params.h`, `services/network/public/cpp/client_hints.h` |
| `sec-ch-prefers-color-scheme` | In a normal profile, the colour mode of Chrome's own theme, which by default follows the app mode of Windows, `AppsUseLightTheme`. In Incognito, the app mode itself. From Chrome 157, a contrast theme decides it instead | `light` or `dark` | `chrome/browser/chrome_content_browser_client.cc` (`GetPreferredColorScheme`), `ui/native_theme/os_settings_provider_win.cc` |
| `sec-ch-prefers-reduced-motion` | Windows' animation effects, `SPI_GETCLIENTAREAANIMATION` | `no-preference`, or `reduce` with them off | `ui/gfx/animation/animation_win.cc`, `content/browser/web_contents/web_contents_impl.cc` |
| `sec-ch-prefers-reduced-transparency` | Windows' transparency effects, `EnableTransparency` in the user's registry | `no-preference`, or `reduce` with them off | `ui/native_theme/os_settings_provider_win.cc` |
| `sec-ch-ua-platform-version` | The version of `Windows.Foundation.UniversalApiContract` in the registry, as `MAJOR.MINOR.0`. Windows 10 before 1809 gets it from a table in the code, and a registry that cannot be read gives `19.0.0` | [One value for each Windows](#which-windows) | `components/embedder_support/user_agent_utils.cc` (`GetUniversalApiContractVersion`) |
| `sec-ch-ua-arch`, `sec-ch-ua-bitness`, `sec-ch-ua-wow64` | The processor architecture of Windows, and whether Chrome is a 32-bit build on 64-bit Windows | `"x86"`, `"64"` and `?0` for 64-bit Chrome on x64 Windows. `"arm"` on Windows on Arm, also for an x64 Chrome there. `?1` for 32-bit Chrome on x64 Windows, still with `"64"` | `components/embedder_support/user_agent_utils.cc` (`GetCpuArchitecture`, `GetCpuBitness`, `IsWoW64`) |
| `sec-ch-ua-model`, `sec-ch-ua-form-factors` | Nothing of the machine | `""` and `"Desktop"` on every Windows machine | `components/embedder_support/user_agent_utils.cc` (`BuildModelInfo`, `GetFormFactorsClientHint`) |

Chrome reads the memory, the platform version and the architecture once in each run, the first time it needs them. The other values follow the machine as it changes, such as a window that is made larger.

### The noise on `rtt` and `downlink`

Before it caps and rounds, Chrome multiplies the estimate by `0.9 + ((hash + salt) mod 21) × 0.01`, one of 21 values from 0.90 to 1.10, where the hash is of the host of the request's URL. It does this in two places, which give a host two different numbers:

| | A navigation, on each hop | A script, image or fetch |
| --- | --- | --- |
| Where | The browser, in `content/browser/client_hints/client_hints.cc` | The renderer of the page, in `third_party/blink/renderer/platform/network/network_state_notifier.cc` |
| The hash of the host | libc++'s `std::hash<std::string>`, cut to 32 bits | Blink's hash of a string, `StringImpl::GetHash()`, which has 24 bits |
| The salt | 1 to 21, drawn once in each run of the browser | 1 to 20, drawn once in each renderer process |

So one host gets one value for its navigations and often another for the subresources of its pages, and both change when Chrome starts again.

A Session does the same, in `native/network_quality.cc`, which ports `GetRandomMultiplier`, `RoundRtt` and `RoundKbpsToMbps` of the browser and `GetRandomMultiplier`, `RoundRtt` and `RoundMbps` of the renderer with their names:

| | A navigation | A script, image or fetch |
| --- | --- | --- |
| When | At its start, on each hop for the host of that hop, and on a restart for `Critical-CH` or for the frame in ALPS for the host it goes to | At its start, for the host it starts at. A redirect keeps them, as Blink does, or takes them away at another origin |
| The hash | libc++'s own `std::hash`, which the native core is built with as Chrome is, with the same ABI version on every platform | Blink's rapidhash of a string, in `native/string_hasher.cc`, which the order of [a subresource's lines](parity.md#the-order-of-a-subresources-lines) also takes |
| The salt | One for the Session, from 1 to 21 | One for each site of a page, from 1 to 20, which stands for its renderer process |

A Session draws the salts each time it opens: from the seed of its Machine if it has one, so that the same Machine sends the same values again, and at random otherwise. The browser's salt reaches the native core as a Session setting, with the estimate. A renderer's comes from a key that the Session draws beside it, and from the site of the page, so that a page of a new site gets a salt of its own without the native core holding any. The hop after a restart for `Critical-CH` keeps the restart's values for the hints that its own origin did not ask for, [as it keeps those hints](parity.md#starting-over), and has its own for the rest. A value that a caller gives in `headers` goes as given, on each hop.

`tests/test_client_hints.py` checks each of these against a port of the same code in Python, `tests/network_noise.py`, on hosts of each length that the two hashes read in a way of their own, and with the cases of `content/browser/client_hints/client_hints_unittest.cc`. The native core calls the libc++ that Chrome builds with, so the port in Python of its CityHash64 is checked there too, and `tests/test_network_noise.py` checks the rapidhash there against the values that Blink's own tests pin.

What the estimator has measured depends on the network. It keeps an estimate for each network it has seen, in the profile. On a network it has not seen, it starts from typical values for the kind of connection until requests have been measured: 90 ms and 1456 kbps on Ethernet, 116 ms and 2658 kbps on Wi-Fi, and 115 ms and 1961 kbps when it cannot tell, in `net/nqe/network_quality_estimator_params.cc`. A field trial can change them. So a new Chrome on Wi-Fi sends `rtt` of `100` or `150`, `downlink` from `2.4` to `2.9` and `ect` of `4g`, until it has measured more. A Persona's `downlink` of `10` takes an estimate of at least 9.1 to 11.1 Mbps, by the noise for the host.

### Which Windows

Chrome 155, 156 and 157 run on Windows 10 and 11. Anything older counts as obsolete, in `chrome/browser/obsolete_system/obsolete_system_win.cc`. So `sec-ch-ua-platform-version` can be any value of Windows 10 or 11. It names the contract, and not the build, so one value stands for several versions of Windows:

| Windows | Build | `sec-ch-ua-platform-version` | From |
| --- | --- | --- | --- |
| 10, 1507 to 1803 | 10240 to 17134 | `"1.0.0"` to `"6.0.0"` | Chrome's own table |
| 10, 1809 | 17763 | `"7.0.0"` | Microsoft |
| 10, 1903 and 1909 | 18362, 18363 | `"8.0.0"` | Microsoft |
| 10, 2004 to 21H2 | 19041 to 19044 | `"10.0.0"` | Microsoft |
| 10, 22H2 | 19045 | `"10.0.0"` | Not confirmed. It is 2004's build with an enablement package, as 20H2 to 21H2 are |
| 11, 21H2 | 22000 | `"14.0.0"` | Microsoft's SDK for that build |
| 11, 22H2 | 22621 | `"15.0.0"` | Microsoft |
| 11, 23H2 | 22631 | `"15.0.0"` | Not confirmed. Microsoft gives it 22H2's SDK |
| 11, 24H2 | 26100 | `"19.0.0"` | Microsoft |
| 11, 25H2 | 26200 | `"19.0.0"` | Not confirmed. It is 24H2's build with an enablement package |
| 11, 26H2 | 26300 | `"19.0.0"` | Read from the registry, on 2026-11-08 |
| 11, 26H1 | 28000 | Not known | Its SDK adds contract 20 as experimental only. It only comes on new devices, not as an update |

Microsoft's sources are its page on [detecting Windows 11 through client hints](https://learn.microsoft.com/en-us/microsoft-edge/web-platform/how-to-detect-win11), which stops at 21H2 for Windows 10 and gives Windows 11 as "13 and above", the contract versions of each SDK in [its API catalog](https://apisof.net/), the pages of APIs that came with 24H2 such as [`UserAgeRange`](https://learn.microsoft.com/en-us/uwp/api/windows.system.useragerange), and [its release information](https://learn.microsoft.com/en-us/windows/release-health/windows11-release-information) for builds and dates, all read on 2026-11-08. Reading the registry on each version would confirm the rest. The Windows that recorded the Baselines says `"19.0.0"`, as [Windows 11 24H2 does](#what-they-say), and the normal form hides it.

### What goes together

- **The display, its scale and the window.** A viewport is at most as wide as the display in device-independent pixels, which is its pixels over the scale, and a window that fills the display is exactly that wide. Its height is at most the work area over the scale, less Chrome's bars. The taskbar and Chrome's bars keep their size in device-independent pixels at any scale, give or take a pixel of rounding ([as read](#the-taskbar-and-chromes-bars)), so a scale and a viewport have to be drawn with a display, and not apart from it. For example, a 1920 by 1080 display at 125% is 1536 by 864, and a window that fills it is 1536 wide.
- **The Windows version and the viewport's height.** Windows 10 and Windows 11 have taskbars of different heights, 40 and 48, so a window that fills the display has a height that follows the version. [The heights below](#the-taskbar-and-chromes-bars) say how each was found.
- **The round trip, `ect` and `downlink`.** `ect` follows the round trip with no noise, so `4g` goes with an `rtt` of at most 300, and `3g` with 250 to 1550. The throughput is an estimate of its own, which `ect` does not read.
- **The Windows version and the Chrome Release.** Every Windows 10 and 11 runs every Chrome Release in the Supported Window, so the Persona does not limit the platform version. How common each version is does.
- **On their own.** The memory, the colour scheme, reduced motion and reduced transparency are each apart from the rest. The memory goes loosely with the kind of machine, and so with its display, but no public statistics join the two. The three settings are a user's own.
- **The zoom.** Chrome keeps a zoom for each host, and a default. It multiplies `dpr` and divides the viewport, as the page would. A machine at 100% is the common case, and a zoom for each host would be a setting of its own.

#### The taskbar and Chrome's bars

A window that fills the display is as wide as the work area, and its page area is the work area's height less Chrome's bars. Chrome's first window on a new profile, with no size saved, sits 10 from the work area's top left corner and is as high as the work area less 20. It is as wide as half the work area less 15 when the primary display is 16:10 or wider and the work area is wider than 1600, and otherwise as wide as the work area less 20, at most 1050. On a display in portrait, a window that would be higher than wide is three quarters of its width high instead (`chrome/browser/ui/window_sizer/window_sizer.cc`, `GetDefaultWindowBounds`, with `kWindowTilePixels` and `kWindowMaxDefaultWidth` in `window_sizer_aura.cc`). So a 1920 by 1080 display at 125%, with a work area 1536 wide, gets a first window 1050 wide. Its page area is the window less the bars and the window's border. Each value here is in device-independent pixels, as `screen` and a page's sizes have them:

| Windows | Scale | Taskbar | Bars, maximised | Bars and top border, first window | Side borders, first window | How |
| --- | --- | --- | --- | --- | --- | --- |
| 11 | 100% | 48 | 87 | 95 | 16 | Read |
| 11 | 125% | 48 | 86 | 95 | 14 | Read |
| 11 | 150% | 48 | 87 | 94 | 15 | Read |
| 11 | 200% | 48 | 87 | 93 | 13 | Read |
| 10 | 100% | 40 | 87 | 95 | 16 | Read on Windows Server 2022. Not confirmed on Windows 10 22H2 |
| 10 | 125% and up | Not read | Taken from Windows 11 | Taken from Windows 11 | Taken from Windows 11 | The same code draws Chrome's bars on both. Not confirmed |

The taskbar is `screen.height - screen.availHeight`, the bars are `outerHeight - innerHeight`, and the side borders are `outerWidth - innerWidth`. A maximised window has no border. So a window that fills a 1920 by 1080 display on Windows 11 at 100% has a viewport of 1920 by 945, which is what [the Persona claims](#what-they-say) today.

How each was read, on 2026-11-08:

- **Windows 11.** Chrome 157.0.8103.39 on Windows 11 26H2, build 26300, with the taskbar as Windows sets it up, at the bottom and not hidden. At each scale, a new profile read its first window, then the same window maximised. The same profile, restarted once its variations seed had arrived, gave the same bars when maximised, so the field trials of that seed change none of them. A restarted Chrome restores the window it had, which at 125% came back a pixel or two off the first window's size, so only a first start gives the first window's values.
- **Windows 11 24H2.** A hosted `windows-2025` runner, whose Windows Server 2025 is build 26100, with Chrome 156.0.8078.80, at 100% only. It gave the same as 26H2.
- **Windows 10.** A hosted `windows-2022` runner, whose Windows Server 2022 has the taskbar of Windows 10, with Chrome 156.0.8078.80, at 100% only. Chrome drew the same bars and borders there as on Windows 11.

The Windows 11 readings had work areas wider than 1600, so their first windows were half as wide. The runners' work areas were 1024 wide, and their first windows 1004, the other case.

Chromium 157's source gives the bars of a maximised window as 87, at any scale: a tab strip of 41, which is a tab of 34, 1 of overlap with the toolbar and 6 of padding (`chrome/browser/ui/layout_constants.cc`, `kTabHeight`, `kTabStripPadding`), and a toolbar of 46, which is a button of 34 with a margin of 6 above and below (`kToolbarButtonHeight`, `TOOLBAR_INTERIOR_MARGIN`). With the tab strip shown, Chrome draws its own title bar on Windows 10 and 11 alike, and a maximised window has no top border (`chrome/browser/ui/views/frame/browser_frame_view_win.cc`, `FrameTopBorderThickness`, `TopAreaHeight`). The readings agree, except that at 125% the page area came out a pixel higher, from rounding to whole pixels. The first window's size came out up to 2 larger than `GetDefaultWindowBounds` gives at 125% and 150%, for the same reason. What the source cannot give is the taskbar, which is Windows', and the border of a window that is not maximised, which comes from Windows' system metrics in pixels, such as `SM_CXSIZEFRAME`, so it changes with the scale. A field trial such as `IsToolbarGlowUpEnabled` could still change the bars for some users.

### How common each one is

These are the public statistics found on 2026-11-08, with the latest period each had then. None of them splits by browser and Windows together, and none measures what Chrome sends, so each weight is a step away from it.

| What | Source and period | What it says |
| --- | --- | --- |
| Windows 10 against 11 | [StatCounter](https://gs.statcounter.com/windows-version-market-share/desktop/worldwide), desktop, worldwide, October 2026 | Windows 11 74.2%, Windows 10 25.2%, older 0.6%. It moves by several points from month to month |
| | [Steam's survey](https://store.steampowered.com/hwsurvey/?platform=pc), Windows, October 2026 | Windows 11 77.4%, Windows 10 22.5% |
| | [Firefox Public Data Report](https://data.firefox.com/dashboard/hardware), week of 2026-10-26 | Of Windows, about 61% Windows 11, 34% Windows 10 and 5% Windows 7 |
| Each version of Windows 11 | None | No public source splits Windows 11 by version. AdDuplex, which did, closed in 2023 |
| The size of the display, in CSS pixels | StatCounter, [desktop screen resolution](https://gs.statcounter.com/screen-resolution-stats/desktop/worldwide), October 2026, all desktop systems | 1920×1080 27.8%, 1536×864 9.2%, 800×600 8.7%, 1366×768 7.5%, 1280×720 4.5%, 2560×1440 3.8%, and a long tail. It reads `screen.width` and `screen.height`, so a 1920×1080 display at 125% counts as 1536×864. macOS is in it, and 800×600 looks like machines that are not people's |
| The size of the display, in pixels | Steam, Windows, October 2026 | 1920×1080 51.9%, 2560×1440 22.5%, 2560×1600 5.8%, 3840×2160 5.1%, 3440×1440 3.2%, 1920×1200 3.0%, 1366×768 2.3%. Machines for games |
| | Firefox, week of 2026-10-26, all desktop systems | 1920×1080 51.2%, 1366×768 13.5%, 1920×1200 5.1%, 2560×1440 4.4%, 1600×900 4.0%, 2880×1800 2.2%, 3840×2160 2.1% |
| The scale | None | No public source gives it. StatCounter's sizes in CSS pixels beside the others' in pixels are the only way to see it: 1536×864 is 1920×1080 at 125%, and 1280×720 can be 1920×1080 at 150% |
| The memory | Firefox, week of 2026-10-26, all desktop systems | In Chrome's values, worked out here from Firefox's rounded sizes: `2` 3.2%, `4` 12.4%, `8` 34.5%, `16` 33.2%, `32` 12.3%, and 4.4% that Firefox does not name |
| | Steam, Windows, October 2026 | 16 GB 41.3%, 32 GB 38.2%, 8 GB 7.0%. Machines for games |
| Dark mode | [Chrome Platform Status](https://chromestatus.com/metrics/feature/timeline/popularity/3581), `PreferredColorSchemeDarkSetting`, early November 2026 | About 40% of page loads, on every platform with Android, which is not Windows alone |
| Reduced motion and transparency | None | Only how many pages ask for them, not how many users turn them on |
| The round trip and throughput | [Chrome UX Report](https://developer.chrome.com/docs/crux/bigquery) | A round trip in three bins for each origin, country and kind of device, in BigQuery. A table for all desktops would have to be worked out from it there. Not fetched |

Firefox rounds the memory to whole GB, so for Chrome's values 3 GB counts as `2`, 6 GB as `4`, 7 and 12 GB as `8`, 14, 15 and 24 GB as `16`, and 31 and 64 GB as `32`. 6 and 12 GB sit on Chrome's ties, so a few of them would go up instead.

Where a public source has nothing, a Machine can still take what most users have: Windows ships with animations and transparency on, so `no-preference` for both is what most real machines say, and a value that most users share puts no Session on a point of its own.

#### Licences

| Source | Licence | Can the repository ship a table made from it? |
| --- | --- | --- |
| StatCounter | [CC BY-SA 3.0](https://gs.statcounter.com/faq), with a link to them wherever the data is used | Yes, as a file of its own under CC BY-SA 3.0 with that credit. The code keeps Apache-2.0, and the wheel then declares both |
| Firefox Public Data Report | The site says its content is under a Creative Commons licence, which Mozilla's [licensing page](https://www.mozilla.org/en-US/foundation/licensing/website-content/) gives as CC BY-SA 3.0 or later. The data files carry none of their own | As for StatCounter, though only Mozilla can say for certain that the data is covered |
| Chrome UX Report | [CC BY 4.0](https://developer.chrome.com/docs/crux/methodology) | Yes, with credit |
| Chrome Platform Status | None found for its data | No |
| Steam's survey | None, and Valve's terms keep all rights | No. It can only check what the others say |
| Cloudflare Radar | [CC BY-NC 4.0](https://developers.cloudflare.com/radar/) | No, since Apache-2.0 allows commercial use |

This is a reading of their terms, not legal advice. A table under a share-alike licence beside Apache-2.0 code is a decision for the owner.

#### Keeping the table current

StatCounter has a month on the second of the next and revises it for 45 days, through a CSV link that its page uses but does not document. Firefox's report is weekly, at stable JSON addresses. The Chrome UX Report is monthly, in BigQuery, and needs a Google Cloud account. So [a step of the release routine](releasing.md#7-refresh-the-table-of-machines) fetches StatCounter's newest month that it no longer revises and Firefox's newest week, writes the table again and shows its diff for review. Steam's survey has no API and is no source for the table.

### The table

[`src/mitto/machine_table.json`](../src/mitto/machine_table.json) holds a group of weighted rows for each set of values that are drawn together. [`tools/release/machines.py`](../tools/release/machines.py) writes it from the statistics above, which [`tools/release/machine_statistics.json`](../tools/release/machine_statistics.json) holds with their sources and periods, and the owner's weights. [The release routine](releasing.md#7-refresh-the-table-of-machines) fetches the statistics again and writes both files. This writes the table from the statistics as they are, and fetches nothing:

```bash
uv run python -m tools.release machines --saved
```

| Group | Its rows | From |
| --- | --- | --- |
| `platform_version` | `"19.0.0"` for Windows 11 and `"10.0.0"` for Windows 10, each with which Windows it is | StatCounter's shares of Windows 11 and 10. Older Windows cannot run Chrome 155 to 157, so it counts for nothing |
| `display` | The display in pixels, its scale, which window it has and that window's viewport, for each Windows | Firefox's sizes in pixels, each split between 100%, 125%, 150% and 200% by how common StatCounter finds the size that each scale gives in CSS pixels. A size in CSS pixels that more than one display gives, such as 1280×720 from 1920×1080 at 150% and from 2560×1440 at 200%, is shared out between them by Firefox's shares. A size in pixels that gives no size StatCounter names at any of them is at 100%. Then the owner's weights for the window: 70% maximised and 30% Chrome's first window on a new profile. Each Windows has the same displays and windows at the same weights, and its own taskbar in the viewport |
| `memory` | 2, 4, 8, 16 and 32 GiB | Firefox's sizes in whole GB, each counted as what Chrome sends for that many GiB, as above |
| `network` | Chrome's starting values for Wi-Fi and for Ethernet | The owner's weights: 60% Wi-Fi and 40% Ethernet |
| `color_scheme` | `light` and `dark` | The owner's weights: 30% `dark` |
| `reduced_motion`, `reduced_transparency` | `no-preference` | The owner's: what Windows comes with |

The viewport follows from the display, the scale, the window and the Windows, by [the taskbar and Chrome's bars](#the-taskbar-and-chromes-bars): a maximised window has the work area less Chrome's bars, and Chrome's first window on a new profile has the size that `GetDefaultWindowBounds()` gives it, less its bars and borders. Windows 10 at 125% and up takes the bars and borders of Windows 11 and its own taskbar of 40, as that table does. Windows offers more scales than the four read there, such as 175% and 250%, which wait for a reading of Chrome's bars, so a display's share goes to those four.

A source's "Other", StatCounter's 800×600 and its sizes that only another system gives, such as 1470×956 of a Mac, count for nothing, because no size in pixels from Firefox gives them at a scale Windows offers. The weights of each group are whole numbers out of 1,000,000, so that a draw needs no floating point, and a row whose weight rounds to nothing is left out. The table has one line for each row, so that a change of weight is one line of its diff. It holds its licence and the credit of each source, and [`MACHINE_TABLE_NOTICE`](../MACHINE_TABLE_NOTICE) goes with it in the wheel.

[`Machine.draw()`](api.md#machinedraw) draws the groups in the order above. Each group takes a number of its own from the SHA-256 of the seed and the group's name, and the row that number lands on when each row has as many numbers as its weight, among the rows that fit what is fixed and drawn already. So a value fixed in one group leaves the others as the seed draws them, unless they have to fit together, and the same seed and table give the same Machine on every Host and in every Python. A row of the platform version only goes if a display fits it, so a viewport of one Windows draws that Windows. A Machine checks each value against [what Chrome can read](#what-decides-each-value), and that the viewport fits on the display.

[`tests/test_machine.py`](../tests/test_machine.py) checks that every value of a draw is one Chrome sends and that each row keeps to [what goes together](#what-goes-together), [`tests/release/test_write_machines.py`](../tests/release/test_write_machines.py) that the script writes the table byte for byte, and [`tests/release/test_refresh_machines.py`](../tests/release/test_refresh_machines.py) that a refresh takes the periods that [the release routine](releasing.md#7-refresh-the-table-of-machines) names and changes nothing when a source fails, with answers that the sources gave on 2026-11-08.

### Where it lives

[ADR-0007](adr/0007-a-machine-of-its-own-per-session.md) gives each Session a machine of its own, apart from its Persona, which stays one for each Chrome Release:

| Question | Decision | Because |
| --- | --- | --- |
| Drawn or fixed? | Drawn by default, from a table that ships with mitto. A caller can fix any value, or give the whole machine | A fixed default leaves every Session that does not ask on one point, which is the problem. A machine that the caller must always give would mostly be copied from one example, with the same result |
| Repeatable? | The same seed gives the same machine with the same table. The machine keeps its seed, and a later mitto that knows more fields draws the new ones from it, to fit those it has | Tests need the same values twice, and a saved machine must not change when mitto learns more |
| Saved? | In `cookie_file` and by [`Cookies.save()`](api.md#cookiessave), beside the cookies. A Session that loads the file is that machine | A returning user whose machine changed between visits would stand out |
| A file from mitto 0.1? | It has no machine, so it loads with the one every Session claimed then, the values [above](#what-they-say) | A returning Session must not change machine because mitto changed |
| What it holds | The memory, the display with its scale, the viewport, the round trip and throughput as estimated before noise, the three settings and the Windows version | What Chrome reads, and not what it sends, so that noise can be added for each host and a page can later read the same machine |
| What it leaves to the Persona | `sec-ch-ua-arch`, `sec-ch-ua-bitness`, `sec-ch-ua-wow64`, `sec-ch-ua-model` and `sec-ch-ua-form-factors` | They are the same on every x64 Windows machine. Windows on Arm would need Baselines of its own first, to show that nothing else changes |
| The salts of the noise | Drawn each time a Session opens, from its seed if it has one, and never saved | Chrome draws them again each time it starts, so a returning user has new ones too. A seed gives the same ones again, so that a test sees the same values |
| On a Windows **Host** | Drawn as on every other Host, and never read from the machine mitto runs on | The owner decided this on 2026-11-08 |

Who writes each value: a Session writes its Machine's values into the Persona's lines when it opens, in place of `<machine>`, as the Persona's own values go. `rtt` and `downlink` change with the host, so the native core works them out on each hop, from the estimate and the salts that reach it as Session settings, [as Chrome does](#the-noise-on-rtt-and-downlink). The Persona's lines hold their places with the values of no noise.

The native core lists the cookies as `{"version": 1, "cookies": [...]}`, and refuses what it does not know ([ADR-0003](adr/0003-c-abi-and-ctypes.md)). So the Python layer writes the cookie file as version 2, with the machine beside the cookies, and hands the native core the cookies alone, as [Saved cookies](parity.md#saved-cookies) says.

### What happens to Parity Checks

A drawn value cannot be compared with a Baseline, so the normal form keeps its line in its place and hides the value as `<machine>`, for each of the 14 hints. `sec-ch-ua-platform-version` is one of them since 2026-11-08, when the Baselines were [written again](baselines.md#the-platform-version-hidden) with that one value changed. The order of the lines stays compared, and each Parity Check claims a Machine drawn for its Session.

What checks the values instead:

- Tests of each rounding against the cases of Chromium's own tests, `approximated_device_memory_unittest.cc` and `content/browser/client_hints/client_hints_unittest.cc`.
- A test that each value a draw can give is one that Chrome can send, by [the table above](#what-decides-each-value), and that a drawn machine keeps to [what goes together](#what-goes-together).
- Tests that a Session sends its machine's values on each hop, with `rtt` and `downlink` worked out for each host.
- [A check of each recording](capture.md#the-form-of-a-hidden-value), where the recordings are kept, that a value the normal form hides has the form a Session's has, and tests that hold a Session's values to the same forms. Real Chrome's formats are then compared, and the recording machine's values still never reach the repository.

### The Runtime

The **Runtime** will have to read the same machine, or a page could tell that its scripts and its requests disagree. What a page can read, and what it needs:

| A page reads | From the machine |
| --- | --- |
| `navigator.deviceMemory` | The same memory |
| `devicePixelRatio`, and `resolution` in `matchMedia()` | The scale, times the zoom |
| `innerWidth`, `innerHeight`, and `width` and `height` in `matchMedia()` | The viewport |
| `screen.width`, `screen.height` | The display, over the scale |
| `screen.availWidth`, `screen.availHeight` | The work area, over the scale |
| `outerWidth`, `outerHeight`, `screenX`, `screenY` | The window: the viewport with Chrome's bars, and where it is |
| `prefers-color-scheme`, `prefers-reduced-motion` and `prefers-reduced-transparency` in `matchMedia()` | The three settings |
| `prefers-contrast`, `forced-colors` and `inverted-colors` in `matchMedia()` | Windows' contrast themes and colour filters, which no hint sends |
| `navigator.connection` | The same estimate, with the renderer's noise for the page's host, in `third_party/blink/renderer/modules/netinfo/network_information.cc` |
| `navigator.userAgentData.getHighEntropyValues()` | The platform version, and what the Persona holds |
| `navigator.hardwareConcurrency`, `navigator.maxTouchPoints`, the GPU through WebGL, the time zone | Nothing yet |

So the shape has to allow three things. It keeps what Chrome reads, such as the display, its scale and the work area, and not only the viewport, so that `screen` and the window agree with the hints. It keeps the estimate before noise, because a page and a request add different noise. And it takes new fields: a machine saved by one version of mitto loads in a later one that knows more, which draws what is missing from the machine's seed to fit what it has.

### What changes for callers

- A Session that does not fix its machine sends values of its own to an origin that asks for them, and another Session sends others. A server that does not ask sees no change. [`Machine.named("mitto-0.1")`](api.md#machinenamed) gives the machine of mitto 0.1 back.
- `sec-ch-ua-platform-version` stops being `"19.0.0"` for everyone.
- A `cookie_file` written by the new version holds the machine as well, and mitto 0.1 refuses it, because its native core does not know the new version of the file. A file from mitto 0.1 loads in the new version as the machine above.
- So this ships in a new minor version, as [a run](releasing.md#when-a-run-starts) raises it, and not in a patch. Its release notes have to say so.
