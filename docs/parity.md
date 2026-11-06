# Parity Checks

A **Parity Check** compares what the public API sends through the native core with what real Chrome sent, as recorded in a **Baseline**. Everything milestones M1 and M2 do is measured with them. Terms in **bold** are defined in [CONTEXT.md](../CONTEXT.md).

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
2. creates a new `mitto.Session` with the **Persona** of the Baseline's **Chrome Release**. It accepts the server's private certificate and finds the names of the case pages, through settings that only the private `_settings` argument reaches. For an `h3` Baseline it also forces QUIC on the server's HTTP/3 origins, as the Chrome that recorded it was told to, because `//net` refuses QUIC to a private certificate otherwise. For a Baseline recorded through the capture server's proxy it has that proxy, and then the proxy is what finds the names;
3. [replays](#replaying-a-recording-case) the Baseline's **Recording Case** through the public API, with what the Persona sends for that case's **Request Kind** ([persona.md](persona.md));
4. takes the normal form of what the server received, and compares it with the Baseline.

Each comparison covers what the connection looks like and the one request. So a difference in the connection, such as in the ClientHello, shows up once for each Recording Case. The Baselines hold the same connection in every file of a protocol, but a replay need not, so this still says something about the native core.

The native core of the **Engine Release** presents every Chrome Release in turn, so one native library checks all of them. A Baseline of a Chrome Release that has no Persona stops the run.

## Baselines

A Baseline file is the normal form the capture kit writes, at `tests/baselines/RELEASE/NAME.json` for the main **Request Kinds** and `tests/baselines/RELEASE/cases/NAME.json` for the other **Recording Cases**, where `RELEASE` is the **Chrome Release**. The file holds one Baseline for each Recording Case in it, and the protocol comes from the file's `protocol` key. The case and the protocol together say which [run](capture.md#recording-cases) recorded it, so its route into the server and the name it starts at. [baselines.md](baselines.md) is how to record them.

Other files under `RELEASE` are left alone: `*.raw.json` and anything that does not end in `.json`, such as `chrome.txt`, which says which Chrome recorded the Baselines.

## Replaying a Recording Case

A replay does what a caller of the public API can do today, in `REPLAYS` in `tools/parity/replay.py`: the case's method and **Request Kind**, at the case's name, on the route of its run. It passes no headers of its own, so what the Persona and `//net` send is what gets compared. What the API cannot do yet is therefore a set of gaps, which go when the API can do it:

| Recording Cases | A replay asks for | What it cannot do yet, and how that shows |
|---|---|---|
| `navigation`, `fetch`, `script`, `image` | The case's path at the server's own name, with no **Initiator** | Nothing |
| `navigation_link_*`, `fetch_get_*`, `script_*`, `image_*` | The case's path at its name, from the page it came from | Nothing |
| `navigation_form_*`, `fetch_post_text_*`, `fetch_post_json_same_origin` | `POST` with the case's body, from the same fields as the page's form or script, and from the page it came from | Nothing |
| `fetch_post_stream` | `POST` with no body, from the page it came from | Send a stream. Over HTTP/1.1 the Baseline is `{"refused": true}`, so everything the replay sends is a gap. A replay that fails with `ERR_ALPN_NEGOTIATION_FAILED` and leaves nothing on the wire matches it |
| `cookie` | `/case/cookie-set`, then the case's path, both from the page it came from | Nothing. A case counts a request without what it should carry when none has it, so a Session that lost its cookie would show one gap in `Cookie` |
| `revalidation` | The case's path twice, from the page it came from | Keep a response, so `If-None-Match` is a gap in the same way |
| `redirect_navigation_*`, `redirect_fetch_*` | The first hop, whichever hop the Baseline is, from the page it came from. `//net` follows the rest, and the native core changes the headers on each | Nothing |
| `plain_http_*` | The case's path over plain HTTP, from the page it came from, or with no Initiator for the navigation | Nothing |
| `proxy_connect`, `proxy_navigation` | The path of `proxy_navigation` at its name, through the capture server's proxy, which also makes the `CONNECT` | Nothing |

The page of each case is `page` in its `Replay`, from the pages in `tools/capture/cases.py`, and the Baselines' `Referer` lines say the same.

When a **Session** gains what a case needs, change its replay to pass it on, and write the list again. `cookie` and the hops of a redirect need no change, because they already make the requests that show it. `revalidation` needs a Session with a cache, since a Session has none by default.

## Gaps

A gap is one field in which the two normal forms differ. A failure names the Chrome Release, Recording Case, protocol and field, with both values:

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

A request that gets no response has nothing to compare. It is one gap in the field `response`, whose value for mitto is the net error. A request that never reached the server, with no error, is one gap in the field `request`, whose value for the Baseline is the whole request.

## The known-gaps list

`tests/baselines/known_gaps.json` holds every **Known Gap**, one per line, with the keys `release`, `case`, `protocol`, `field`, `baseline` and `mitto`. Here `null` means that side does not have the field. `case` is the name of the Recording Case, so a case with no line in the list matches its Baselines, and a line holds what to look for: `grep '"case": "cookie"' tests/baselines/known_gaps.json`.

A run fails on a gap that is not in the list and on a gap in the list that no longer exists. A gap whose value has changed counts as both, so the list always says exactly how mitto differs from the Baselines.

There is one list for both **Hosts**, because a Wire Fingerprint must not depend on the Host. A gap on only one Host fails the run on the other.

To write the list again from what the native core does now:

```bash
uv run python -m tools.parity
```

This needs a native library, and it runs the same comparison as the tests. It also prints how many gaps each Recording Case has, over every Chrome Release and protocol, where 0 means the case matches. `--baselines DIR` uses another directory of Baselines, with its own list. Read the diff before you commit it. Fixing a gap only removes lines. An added line means mitto moved away from a Baseline, or a Baseline changed.

## What the native core states

Real Chrome gets its settings from preferences, field trials and the defaults in `//net`, and a source build has only the defaults ([ADR-0001](adr/0001-chromium-network-stack.md)). So the native core sets them itself, in code ported from Chrome with its names kept:

| File | States | Upstream |
|---|---|---|
| `native/network_session_configurator.cc` | HTTP/2 connection settings, what its connections do on a network change, and with `enable_http2` the ALPN list. QUIC parameters, and the origins that a field trial tells Chrome speak QUIC | `components/network_session_configurator`, and the hints in `net/http/http_server_properties.cc` |
| `native/ssl_config_service_mojo.cc` | TLS versions, cipher suites, key shares, ECH GREASE and trust anchors | `chrome/browser/ssl/ssl_config_service_manager.cc`, `services/network/ssl_config_service_mojo.cc` and `ssl_config_type_converter.cc` |
| `native/feature_list.cc` | Features that the ClientHello depends on, over TCP and QUIC, and those in `//net` that field trials turn on | The `--enable-features` and `--disable-features` lists in `chrome.txt` |
| `native/fetch_rules.cc`, `native/sec_header_helpers.cc` | [The headers that follow the page a request comes from](#the-initiator-and-each-hop), and what `//net` is told about that page | `SetFetchMetadataHeaders` and `MaybeRemoveSecHeaders` in `services/network/sec_header_helpers.cc`, `CorsURLLoader` and `URLLoader` in `services/network`, and the parts of the browser process and Blink that the section names |
| `native/mitto_core.cc` | That network state is partitioned, in [the call Chrome makes](#partitioning) | `ChromeMainDelegate::CommonEarlyInitialization()` |
| `native/stub_resolver_config_reader.cc` | The stub resolver: the built-in DNS client, additional query types, which is what asks for HTTPS records, secure mode automatic, and the upgrade to DNS over HTTPS | `ConfigureStubHostResolver` in `services/network/network_service.cc`, with the defaults in `chrome/browser/net/stub_resolver_config_reader.cc` |

A value stated here stays when a new **Engine Release** changes a default, so a Parity Check fails instead of mitto moving with the engine. That matters because an older **Chrome Release** must keep sending what it sent. All of this holds for every Persona, since Chrome 155, 156 and 157 [send the same in it](persona.md#what-differs-between-chrome-releases).

When new Baselines list a feature in `chrome.txt` that the native core's build holds, add it to `native/feature_list.cc`. A feature outside that build, such as one in `//content`, does nothing in the native core. `chrome.txt` holds only names that look network-related, and a recording draws one group of each study, so a study with a small group may not show. The variations seed itself holds every study.

The trust anchors come from the root store compiled into `//net`, as they do in a Chrome that has just started. mitto has no component updates, so the list stays that of the Engine Release until the next one, whatever a Chrome that has run for a while has moved to ([what can differ](capture.md#what-can-differ-between-chrome-installations)).

`native/mitto_core.cc` creates one `NetworkChangeNotifier` and one `HostResolverManager` per process, as the network service does, and each **Session** has its own cache of answers on that manager. A Session starts DoH probes 5 seconds after the process created them, the default of `kDelayInitialDohProbeTimeoutParam`.

The notifier is also what makes a state of the HTTP/2 connections take effect: `spdy_go_away_on_ip_change`, which `native/network_session_configurator.cc` sets to true. Chrome takes the default of `//net` for it, true on Windows and false on macOS and Linux, and every **Persona** is Windows Chrome, so it would otherwise follow the **Host**. With true, a change of the IP address makes a Session's HTTP/2 connections go away, so that the requests on them can finish and new requests open another connection. With false they would close and fail those requests with `ERR_NETWORK_CHANGED`.

The first Session in a process waits up to a second for the system's DNS settings, which Chrome does not do, because Chrome has them long before a page asks for a host. A program's first request can come right away, and without those settings it would go to the system's resolver, which sees no HTTPS records.

No Parity Check can see any of this. The capture server's names are never looked up, because a `.localhost` name resolves to loopback by itself and a `.test` name goes through `--host-resolver-rules`, so a Baseline's ClientHello always holds ECH GREASE. `tests/test_dns.py` checks real ECH and an `h3` HTTPS record against a DNS server on loopback, which a Session reaches through the `dns_nameserver` setting. Only `mitto._native` offers that setting, and the public Session takes it only through `_settings`. Such a Session has a `HostResolverManager` of its own, so that other Sessions keep the system's settings.

## The Initiator and each hop

What the headers say ([the rules](persona.md#the-initiator)) depends on the page a request comes from and on each URL it goes through. In Chrome several layers decide that. All of them but `//net` are outside the source export, so `native/fetch_rules.cc` and `native/sec_header_helpers.cc` port them, with their names kept, and run them at the start and on each hop:

| Rule | In Chrome | Where |
| --- | --- | --- |
| `Sec-Fetch-*` | `SetFetchMetadataHeaders`, and `MaybeRemoveSecHeaders` on a hop | `services/network/sec_header_helpers.cc` |
| `Origin` on a fetch | `CorsURLLoader::MaybeSetOriginHeader`, `ShouldIncludeOriginHeader` and `CheckTainted` | `services/network/cors/cors_url_loader.cc` |
| Whether a request sends and stores cookies | `url_loader_util::SetRequestCredentials` | `services/network/url_loader_util.cc` |
| `Sec-Fetch-Storage-Access: active` | `CookieSettingsBase::GetStorageAccessStatus`, which a Session answers as a Chrome with no setting that blocks a cookie does | `components/content_settings` |
| `Origin` on a navigation | `AddAdditionalRequestHeaders` and `Referrer::SanitizeOriginForRequest` | `content/browser/renderer_host/navigation_request.cc`, `content/public/common/referrer.cc` |
| `sec-ch-*` moving on a navigation's hop | `NavigationRequest::OnRequestRedirected` removes every client hint and adds those of the next URL as modified headers, and `//net` merges those last | `content/browser/renderer_host/navigation_request.cc` |
| `sec-ch-*` staying on a subresource's hop | `FrameFetchContext::AddClientHintsIfNecessary` runs when a request starts, and not on a redirect | Blink |
| `Referer` of the first hop | `SecurityPolicy::GenerateReferrer`, which `URLRequestJob::ComputeReferrerForPolicy` answers the same way | Blink, and `//net` from the second hop |
| `IsUrlPotentiallyTrustworthy` | Already in `//net` at this Engine Release, so it needs no port | `net/base/is_potentially_trustworthy.cc` |

So the last four rows hold what comes from outside `//net` and `services/network`. `Sec-Fetch-Frame-Top` and `Sec-Fetch-Frame-Ancestors` are behind features that are off by default and in the Baselines, so they are not ported, and no **Request Kind** is a subframe's, so a page always counts as top-level.

A **Persona**'s lines arrive at the native core with the rest of the headers, each in its place, along with what `network::ResourceRequest` would say: `mode`, `destination`, `credentials_mode`, `has_user_activation` and `initiator`. `fixed_headers` names what a caller gave or removed, which no rule touches. A request with none of this, which only `mitto._native` can make, sends what its headers say.

## Partitioning

Chrome partitions connections, TLS sessions, DNS results and more by the top-level site, because `ChromeMainDelegate::CommonEarlyInitialization()` calls `NetworkAnonymizationKey::PartitionByDefault()` ([what was measured](baselines.md#features-on-in-chromes-own-code)). `NetworkCore` makes the same call, right after the feature list exists and before anything can ask whether partitioning is on. Its comment marks the place for the same call that the split cache needs.

Each request has an `IsolationInfo`, as in Chrome: a navigation's follows it as a top frame on each hop, and a subresource's stays that of its page. So over each protocol, a subresource from one top-level site does not share a connection with one from another, and a navigation shares one with what its own page asks of its own site. A cross-origin `fetch()` has no credentials, so `//net` gives it a connection apart from one that has them.

No normal form can show this, so `tests/test_initiator.py` counts the connections at the server, over HTTP/1.1, HTTP/2 and HTTP/3. `Partitioned` cookies go in the partition of the same site.

## Cookies

A **Session** has `CookieMonster`, the cookie store in `//net` that Chrome uses, so `//net` decides which cookies a response sets, which a request sends and in what order. The `cookie` **Recording Case** shows that one reaches the wire in the place Chrome puts it, over each protocol. `tests/test_cookies.py` checks what no **Baseline** holds: `Secure`, `HttpOnly`, `__Host-` and the other prefixes, `Domain`, `Path` and expiry, all through a real response.

`session.cookies` reads and changes that store through the C API, whose JSON [mitto.h](../native/include/mitto/mitto.h) describes. A cookie has no fixed encoding, so its name, value and path hold one character for each byte, as a `Response`'s headers do. Nothing a server sends is lost that way.

A cookie crosses the C API in one of two ways, which are those of `//net`:

| The JSON | `//net` builds it with | Which checks |
| --- | --- | --- |
| Has no `created_us`, so it is new | `CanonicalCookie::CreateSanitizedCookie()`, as DevTools does | Those of a server at its domain: a `__Host-` cookie must be `Secure`, a dot before a public suffix gives a cookie for that one name, and a path is encoded as a URL does |
| Has `created_us`, so it has been in a store | `CanonicalCookie::FromStorage()`, as Chrome's own store does | That it is canonical, which every cookie `//net` has stored is |

Either way, the parser also asks what the store will ask of any cookie, such as whether a `SameSite=None` cookie is `Secure`, so a list with one cookie the store would refuse changes nothing.

The second way is why a saved list always loads, whatever a server sent. The checks of a new cookie refuse a path that grows too long once it is encoded, and they would change the path of any other that a URL has to encode. A cookie stored with such a path never matches a request, and it should stay that way after a load. Either way it counts as coming from `https://` at its domain, so that a caller can replace a `Secure` cookie, and a list that holds a `Secure` cookie and a cookie from HTTP of the same name loads in any order.

### Which site a request belongs to

Chrome gives each request a `site_for_cookies`, an `initiator` and an `isolation_info`, and `//net` decides from those which `SameSite` cookies go. The native core does the same, from `initiator=`:

| Request | `site_for_cookies` and `isolation_info` | `initiator` |
| --- | --- | --- |
| A navigation | Its own URL, as a top frame. `//net` moves them to the next URL on each hop | The page's origin, or none if the browser started it |
| A subresource | The page's site on every hop | The page's origin |

That is all that `//net` needs to apply its rules, which `tests/test_initiator.py` checks through real responses. A cookie is set by a response under the same rule as it goes on a request:

| `SameSite` | Same-site navigation or subresource | Cross-site navigation | Cross-site subresource |
| --- | --- | --- | --- |
| `Strict` | Goes, and can be set | Does not go, and can be set | Neither |
| `Lax` | Goes, and can be set | Goes on `GET`, and can be set | Neither |
| `None` | Goes, and can be set | Goes, and can be set | Goes, and can be set |

A `fetch` has the `credentials_mode` of a page's `fetch()` that does not ask for one, `same-origin`. So once a request of that kind goes to another origin than the page's, it sends and stores no cookie for the rest of its chain, whatever its `SameSite`. A request of any kind with no `initiator=` counts as above, so a navigation sends every cookie of its site, as from the address bar.

`force_main_frame_for_same_site_cookies` is not set. Chrome only sets it on a navigation that a service worker passes on, and `//net` takes a request with a top frame's `isolation_info` as a main frame navigation anyway.

### Saved cookies

`session.cookies.save(path)` writes what the C API lists to a JSON file, in the Python layer. That keeps SQLite out of the build. The file has mode 0600 whatever the umask, and the values are not encrypted. A new file takes the old one's place by a rename, so a crash leaves one or the other whole. `Session(cookie_file=path)` loads when it opens and saves when it closes, so a crash loses what came after the last save. A file that holds anything else stops the Session from opening, because closing it would otherwise overwrite that file.

Session cookies are saved too, as a browser does that restores its last session. A cookie that has expired by the time it loads stays out, because the store removes it on the next read.

### What stays different

| | Chrome | A Session |
| --- | --- | --- |
| A `Secure` cookie on `http://localhost` | `CookieAccessDelegateImpl`, in `services/network`, treats a potentially trustworthy URL as secure, so it can set one and receive it | `//net` with no delegate needs HTTPS |
| A page in a frame | Its `site_for_cookies` can be null, and its partition holds its ancestors | A page always counts as top-level |
| Settings that block a cookie, and First-Party Sets | `CookieSettings` and `FirstPartySetsManager`, in `services/network` | None, which is what a Chrome with no such settings does for a first-party cookie |
| When the store is written | Every 30 seconds, by `SQLitePersistentCookieStore` | When a caller saves, or at close |

## Proxies

`Session(proxy="http://user:password@host:port")`, or with the scheme `https` or `socks5`, sends every request of a **Session** through that proxy. There is no proxy per request. `native/mitto_core.cc` gives `//net` a `ProxyConfigServiceFixed`, with this proxy or with none, so the system's proxy settings and the environment stay ignored. The rules that come of it are those of `--proxy-server` with that one proxy:

- A request to `localhost`, a `*.localhost` name or a loopback address goes around the proxy, because `ProxyBypassRules` has those as implicit rules. So the tests ask for names that exist nowhere, and the proxy finds them.
- A proxy that fails is an error, a `ProxyError`, and never a reason to go directly.
- HTTPS goes through a tunnel, and plain HTTP goes to the proxy with the whole URL. Over `https`, what `//net` says to the proxy is inside TLS, and the proxy's certificate is verified as a server's is, with `additional_trust_anchors` for a private one.
- `//net` uses QUIC only through a QUIC proxy, which a Session does not offer, so a request through a proxy stays on HTTP/1.1 or HTTP/2, whatever `Alt-Svc` says.
- Over `socks5`, the proxy gets the name and finds it.

What `CONNECT` sends comes from `//net`: `Host`, `Proxy-Connection` and `User-Agent`, in that order. A request's headers never reach it, so its `User-Agent` comes from `HttpUserAgentSettings` on the context, which holds the **Persona**'s. A fetch for a certificate takes its `User-Agent` from the same place.

### Credentials

A username and password go in the URL, percent-encoded. As in Chrome, the first `CONNECT` holds none, and they go when the proxy answers with a `407`. Chrome then asks a person, and a Session answers `URLRequest::Delegate::OnAuthRequired` from its settings with `SetAuth()`, so `//net` runs the challenge as it does in Chrome, with whichever scheme the proxy names. It keeps them for the Session's later tunnels, so those send them at once.

A proxy that asks a Session with no credentials, or asks again after their answer, ends the request with `ERR_PROXY_AUTH_REQUESTED`. Chrome would ask a person, and show the proxy's `407` over plain HTTP or `ERR_TUNNEL_CONNECTION_FAILED` through a tunnel if they cancel. A Session has nobody to ask, and a caller needs one error for both. The answer is good once for each hop of a redirect, because the next hop may reach the proxy in another way.

A server's own `401` stays a response, because the proxy's credentials are not for it. A `407` from a server behind a tunnel ends the request with `ERR_UNEXPECTED_PROXY_AUTH`, because `//net` lets only the proxy ask.

A proxy's URL may hold a password, so no message and no `repr` repeats it. A Session does not keep the URL in Python. `SOCKS5ClientSocket` in `//net` offers only "no authentication", so real Chrome cannot use a SOCKS5 proxy that needs credentials, and a `socks5` URL that holds any is refused for now.

## Redirects

`//net` decides what a redirect does, as it does in Chrome: the limit of 20, the method of the next hop and when a body goes. The native core defers each redirect in `OnReceivedRedirect()` and continues it with `FollowDeferredRedirect()`, as `URLLoader` in Chrome's network service does.

It continues from a task of its own, and that shows on the wire. Over HTTP/2 a redirect's `HEADERS` frame usually holds `END_STREAM`, and `//net` tells its delegate about the headers before it handles that. Following the redirect from inside that call would cancel a stream that is about to end, with an `RST_STREAM` that a server can see. Chrome's answer to a redirect comes back from another process, so it sends none.

On each hop it first changes the headers that follow the page a request comes from ([which](#the-initiator-and-each-hop)).

A caller's `Authorization` does not go to another origin, and stays gone if a later hop comes back. This is step 13 of [HTTP-redirect fetch](https://fetch.spec.whatwg.org/#http-redirect-fetch), which `ResourceLoader` does in Blink, so the native core does it in the same place, as a header to remove when it continues. A caller's `Cookie` goes the same way. Fetch does not name it, because a page cannot send one, but it has no domain to keep it where it belongs. The cookies that `//net` adds from a **Session**'s store are not touched, because `//net` decides those on each hop.

`follow_redirects=False` makes a redirect the response, also from a task of its own, so the stream ends as it does when a redirect is followed. Its content is empty because `//net` does not read the body of a redirect, as with `redirect: "manual"` in Fetch. A redirect to a URL that is not valid still ends the request with `ERR_INVALID_REDIRECT`, because `//net` checks that before it tells its delegate.

## Certificate verification

No Parity Check can see how a certificate is verified, because the verdict is not on the wire. A **Session** uses the verifier in `//net` that Chrome uses, with the Chrome Root Store, which at Chrome 157 is the only choice on Windows, macOS and Linux (`chrome_root_store_only`). `native/mitto_core.cc` wraps it in the same `CoalescingCertVerifier` and `CachingCertVerifier` that `NetworkContext` does.

A chain that arrives without an intermediate gets it from the URL in the certificate's Authority Information Access extension. Chrome's fetcher, `CertNetFetcherURLLoader`, lives in `services/cert_verifier`, outside the source export, so a Session has `CertNetFetcherURLRequest` from `//net`. They are written alike: HTTP only, no credentials and no secure DNS, 15 seconds and 64 KiB for an intermediate, and a request on the same context as the page's. A Session's fetcher shuts down with it, which ends a fetch that a worker thread is waiting for.

`Session(additional_trust_anchors=[pem, ...])` adds roots beside the Chrome Root Store, through `CertVerifyProc::InstanceParams`, which is how Chrome receives its own additional certificates. Each entry is the text of a PEM file. A self-signed server certificate can be its own anchor, and an anchor's own dates and constraints are not enforced. This is what mitto offers in place of the roots that Linux users add to the system, which it does not read ([ADR-0006](adr/0006-linux-library-without-system-libraries.md)). On macOS the verifier also reads the roots a user has added to the Keychain, as Chrome does.

`//net` refuses QUIC to a root that is not known, which means one outside the Chrome Root Store, with `ERR_QUIC_CERT_ROOT_NOT_KNOWN`. So a server whose chain ends at an added root stays on HTTP/1.1 or HTTP/2, whatever its `Alt-Svc` says, as it does in Chrome. Only `origins_to_force_quic_on` lifts that, which is what a Parity Check of `h3` uses, and the public Session does not offer it.

### What stays different

| | Chrome | A Session |
|---|---|---|
| Certificate Transparency | `MultiLogCTVerifier` checks SCTs against the logs in `ct_known_logs`, and `ChromeCTPolicyEnforcer` refuses a chain to a known root that has too few, with `ERR_CERTIFICATE_TRANSPARENCY_REQUIRED` | `DoNothingCTVerifier` and `DefaultCTPolicyEnforcer`. The enforcer and the log list live in `components/certificate_transparency`, outside the source export, so a Session accepts a certificate that Chrome would refuse for missing SCTs |
| Revocation | A CRLSet that a component keeps up to date | `CRLSet::BuiltinCRLSet()`, the fixed list compiled into `//net` |
| Root store | The compiled-in list, until a component brings a newer one | The compiled-in list of the **Engine Release** |

These let a Session accept what Chrome would refuse. None of them changes what a server sees: the ClientHello still offers SCTs, and it does not depend on `additional_trust_anchors`. A fetch for a certificate sends the same lines as Chrome 156 on macOS, in the same order: `Host`, `Connection`, `User-Agent`, `Accept-Encoding` and `Accept-Language`. Its `User-Agent` is the **Persona**'s, as Chrome's is its own.

## Tests of the comparison

Real Baselines are recorded by hand on a Windows machine, so `tests/parity` tests the comparison itself with a sample: the real normal forms of Chrome 156 on macOS in `tests/capture/data`. It changes that sample on purpose and expects the run to fail, both through the native core and, where there is no native library, through a stand-in for it. `tests/parity/test_replay.py` checks that each replay goes where its case went, and that a replay compared with itself has no gaps, so that what the list holds is about mitto and not about the replay.
