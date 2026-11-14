# Parity Checks

A **Parity Check** compares what the public API sends through the native core with what real Chrome sent, as recorded in a **Baseline**. Everything milestones M1 and M2 do is measured with them, and what a page sees in a **Page**, its **Script Fingerprint**, with [those of M7](#the-script-fingerprint). Terms in **bold** are defined in [CONTEXT.md](../CONTEXT.md).

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
2. creates a new `mitto.Session` with the **Persona** of the Baseline's **Chrome Release**, and with no `machine=`, so it claims a **Machine** drawn for it, as a caller's Session does. It accepts the server's private certificate and finds the names of the case pages, through settings that only the private `_settings` argument reaches. For an `h3` Baseline it also forces QUIC on the server's HTTP/3 origins, as the Chrome that recorded it was told to, because `//net` refuses QUIC to a private certificate otherwise. For a Baseline recorded through the capture server's proxy it has that proxy, and then the proxy is what finds the names;
3. [replays](#replaying-a-recording-case) the Baseline's **Recording Case** through the public API, with what the Persona sends for that case's **Request Kind** ([persona.md](persona.md));
4. takes the normal form of what the server received, and compares it with the Baseline.

Each comparison covers what the connection looks like and the one request. So a difference in the connection, such as in the ClientHello, shows up once for each Recording Case. The Baselines hold the same connection in every file of a protocol, but a replay need not, so this still says something about the native core.

The native core of the **Engine Release** presents every Chrome Release in turn, so one native library checks all of them. A Baseline of a Chrome Release that has no Persona stops the run.

The test of the real Baselines runs all this twice: through `mitto.Session`, and through a `mitto.AsyncSession` in its place. The two build a request in one place and differ in how they wait, so they should show the same list. `uv run python -m tools.parity` writes the list from `mitto.Session`.

The checks of the Script Fingerprint are tested the same way, with the real Baseline of Chrome 157 changed on purpose: `tests/parity/test_script_gaps.py` holds how each change is named and of which kind, and what the Machine's values miss, with no native library, and `tests/parity/test_script_parity.py` runs them through a real Page.

## Baselines

A Baseline file is the normal form the capture kit writes, at `tests/baselines/RELEASE/NAME.json` for the main **Request Kinds**, `tests/baselines/RELEASE/cases/NAME.json` for the other **Recording Cases** and `tests/baselines/RELEASE/documents/NAME.json` for [what a document requests](baselines.md#documents), where `RELEASE` is the **Chrome Release**. The file holds one Baseline for each Recording Case in it, and the protocol comes from the file's `protocol` key. The case and the protocol together say which [run](capture.md#recording-cases) recorded it, so its route into the server and the name it starts at. [baselines.md](baselines.md) is how to record them.

Other files under `RELEASE` are left alone: `*.raw.json`, anything that does not end in `.json`, such as `chrome.txt`, which says which Chrome recorded the Baselines, and the Baselines of the **Script Fingerprint** in `RELEASE/script/`, which are [Normal Forms](capture.md#the-normal-form-of-a-script-fingerprint) of what the probe records and not of the wire, and which [their own checks](#the-script-fingerprint) read.

A value that [follows the machine](capture.md#what-stays-and-what-does-not), such as `sec-ch-ua-platform-version`, is `<machine>` in a Baseline and in what a Session sent, so a check compares where its line is and not what it says. That is why a drawn Machine passes as any other does. Its form is checked instead, against the same [forms](capture.md#the-form-of-a-hidden-value): `tests/test_machine.py` for every value a Session works out from its Machine, and [`tools.capture.machine_forms`](baselines.md#check-and-commit) for real Chrome's recordings, on the machine that keeps them.

## Replaying a Recording Case

A replay does what a caller of the public API can do today, in `REPLAYS` in `tools/parity/replay.py`: the case's method and **Request Kind**, at the case's name, on the route of its run. It passes no headers of its own, so what the Persona and `//net` send is what gets compared. What the API cannot do yet is therefore a set of gaps, which go when the API can do it:

| Recording Cases | A replay asks for | What it cannot do yet, and how that shows |
|---|---|---|
| `navigation`, `fetch`, `script`, `image` | The case's path at the server's own name, with no **Initiator** | Nothing |
| `navigation_link_*`, `fetch_get_*`, `script_*`, `image_*` | The case's path at its name, from the page it came from | Nothing |
| `navigation_form_*`, `fetch_post_text_*`, `fetch_post_json_same_origin` | `POST` with the case's body, from the same fields as the page's form or script, and from the page it came from | Nothing |
| `fetch_post_stream` | `POST` with the case's bytes as a stream, from the page it came from | Nothing. Over HTTP/1.1 the Baseline is `{"refused": true}`, and the replay fails with `ERR_ALPN_NEGOTIATION_FAILED` and leaves nothing on the wire, which matches it |
| `cookie` | `/case/cookie-set`, then the case's path, both from the page it came from | Nothing. A case counts a request without what it should carry when none has it, so a Session that lost its cookie would show one gap in `Cookie` |
| `revalidation` | The case's path twice, from the page it came from, on a Session with a [cache in a directory](#the-http-cache) | Nothing |
| `redirect_navigation_*`, `redirect_fetch_*` | The first hop, whichever hop the Baseline is, from the page it came from. `//net` follows the rest, and the native core changes the headers on each | Nothing |
| `plain_http_*` | The case's path over plain HTTP, from the page it came from, or with no Initiator for the navigation | Nothing |
| `proxy_connect`, `proxy_navigation` | The path of `proxy_navigation` at its name, through the capture server's proxy, which also makes the `CONNECT` | Nothing |
| `client_hints_*` | A navigation to the page that asks, as `asks` in its `Replay` says, and then the case, as for the cases above | Nothing |
| The cases of documents | The case's path at its name, with the Request Kind of what asked for it, from the page that asked for it, which its `Referer` names: a stylesheet for its `@import` and fonts, a module for what it imports, the cross-site frame for what it asks for. `asks` sets the cookies that a case carries first, as the case's run did | What only a page can say of a request, whose cases a check skips, as [below](#what-a-session-cannot-replay-of-a-document) |

The page of each case is `page` in its `Replay`, from the pages in `tools/capture/cases.py`, and the Baselines' `Referer` lines say the same.

### What a Session cannot replay of a document

A Session replays the cases of documents as a caller would make them. A page makes some of them in ways that a caller cannot say, and a check skips those until a Page makes the request itself, through the private API, as [the Script Fingerprint's checks](#the-script-fingerprint) run. `made_by_a_page` in each such case's `Replay` names the issue that does it, and `python -m tools.parity` lists them as skipped:

| Cases | What a caller cannot say | Who makes a Page make them |
| --- | --- | --- |
| `navigation_location`, `navigation_click`, `navigation_meta_refresh`, `redirect_location_*` | That no user started the navigation, so it has no `Sec-Fetch-User` | [#279](https://github.com/darionxt/mitto/issues/279) |
| `navigation_history_back` | That it traverses history, without a user's activation | [#280](https://github.com/darionxt/mitto/issues/280) |
| `xhr_get_sync`, `xhr_post_text_sync` | That the request is synchronous, which Blink gives the highest priority | [#283](https://github.com/darionxt/mitto/issues/283) |
| `worker_import_scripts`, `worker_fetch`, `worker_blob_fetch` | That a worker asks for it, which sends no hints, at the worker's priority, and with no `Referer` from a `blob:` worker | [#289](https://github.com/darionxt/mitto/issues/289) |
| `frame_script`, `frame_image`, `frame_fetch`, `frame_nested_fetch` | That a frame of another site asks for it, whose site for cookies is no site | [#285](https://github.com/darionxt/mitto/issues/285) |
| `document_*` | The priority that Blink gives what a document's markup asks for, and over HTTP/2 and HTTP/3 the order in which it asks | [#284](https://github.com/darionxt/mitto/issues/284) |

The other cases of documents, the frames' own navigations, `frame_nested` and `favicon` among them, have checks now, and what a Session cannot replay of them yet is in the known-gaps list, as for any case.

`order`, which a normal form of documents holds over HTTP/2 and HTTP/3, belongs to the fingerprint of each case it names, and to no other, so it shows only in the `document_*` cases.

When a **Session** gains what a case needs, change its replay to pass it on, and write the list again. `cookie` and the hops of a redirect need no change, because they already make the requests that show it. Only `revalidation` has a cache, since a Session has none by default, and `tests/parity/test_replay.py` checks that every other case sends the same with one.

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

## The Script Fingerprint

The Parity Checks of M7 compare what a page sees in a **Page** with what it saw in real Chrome: the Normal Form of what [the probe](runtime.md#the-probe) records, against the Baselines in `tests/baselines/RELEASE/script/`, which [baselines.md](baselines.md#the-script-fingerprint) records.

M8 adds [Recording Cases](capture.md#its-recording-cases) in a frame of the page's origin, a `srcdoc` frame, a frame of another site and a dedicated worker. Each of their files keeps [what differs](capture.md#what-a-later-case-keeps) from `page.json` of the same Chrome Release, and a check makes the case's Normal Form again from the two, so that it compares a Page's whole Normal Form in that case, as for the top-level page. A Page has no frames and no workers yet, so a check skips these cases, and `tools.parity --script` says which it skipped, until a Page has frames ([#285](https://github.com/darionxt/mitto/issues/285)) and dedicated workers ([#289](https://github.com/darionxt/mitto/issues/289)) and those set `in_a_page` of the case's run in [`script_page.py`](../tools/capture/script_page.py). Each Chrome Release of the Supported Window has a Baseline of each of them, which [baselines.md](baselines.md#its-baselines-in-frames-and-workers) records, and a case with no Baseline would have no check.

### Run them

They need a native library with the Page program beside it, as a wheel holds them, or as `out/mitto` and `out/runtime` of a build give them once both files are in one directory ([building.md](building.md#build-the-runtime)). `uv run pytest tests/parity` runs them with the wire's checks, and skips them without a Page program. CI's native job runs them on every **Host**, from the installed wheel, through `mitto.Session` and through a `mitto.AsyncSession` in its place, as it runs the wire's.

### What a check does

For each Baseline, it:

1. starts a [capture server](capture.md);
2. creates a new `mitto.Session` with the **Persona** of the Baseline's **Chrome Release** and a **Machine** drawn for it, as the wire's checks do, which accepts the server's private certificate and finds the case pages' names through `_settings`, as Chrome was told to;
3. opens a Page on the Recording Case's top-level page, such as `/script/page` at the case pages' name, through the private `Session._open_page()`, as a caller's Page loads a page. That page runs in the Page as it ran in Chrome: once it has loaded, it runs the probe in a timer through an indirect eval, or has its frame or worker run it, and posts the record to the server with `fetch()`. So nothing reaches into the Page from outside while the probe walks;
4. takes the [Normal Form](capture.md#the-normal-form-of-a-script-fingerprint) of the posted record, and compares it with the Baseline;
5. holds each value that both hide as `<machine>` to the Page's Machine, as [below](#the-machines-values).

A Page takes about a second for all of this, Chrome's three Releases together about three.

### Gaps

A gap is one field in which the two Normal Forms differ, named by the path at which the probe found an object, as `navigator.connection`, and what differs in it:

| Field | What differs | Its values |
| --- | --- | --- |
| `PATH` | An object that only one side reached, such as `navigation.currentEntry` | Its kind, tag and prototype |
| `PATH KEY` | A key of the object, such as `Navigator.prototype tag`, `proto`, `function` or `new` | The key's value |
| `PATH own KEY` | An own property, such as `Navigator.prototype own share` | Its flags and value, or flags, getter and setter |
| `PATH get NAME` | What a getter gave, such as `history get length` | The value |
| `PATH calls NAME` | What a check gave on a new plain object, such as `CookieStore.prototype calls set` | The outcome |
| `PATH own <order>`, `PATH get <order>`, `PATH calls <order>` | The order of the keys both sides have | Each key that moved, with its place among those keys, as a longest run of keys that keep their order leaves them |
| `values["KEY"]["PART"]` | A value of the probe's third step, compared into its objects, such as `values["new Intl.NumberFormat().resolvedOptions()"]["locale"]` | The value, or the whole entry where one side threw |
| `reports` | What the walk made the page report | The reports |

A failure names the Chrome Release, the Recording Case, the kind of gap and the field, with both values:

```text
Chrome 157, page, behaviour, history get length
  Baseline: 1
  mitto:    0
```

### The kind of a gap

Each gap has a kind, which says who closes it. The exit of M7 has Known Gaps for missing behaviour only, and for what a Persona of an older Chrome Release cannot turn off, such as members that its release installed in another order ([persona.md](persona.md#what-an-older-persona-cannot-turn-off)). Any other shape or value is for mitto to match, and the list only holds one until it does:

| Kind | Which gaps |
| --- | --- |
| `machine` | A place that the Normal Form hides as `<machine>` where the case's probe runs (`MACHINE_PLACES` in [`script_normal_form.py`](../tools/capture/script_normal_form.py), and in a frame or a worker its own places), in either comparison. A frame's own viewport, such as its `innerWidth`, is behaviour |
| `persona` | A place that the Persona decides (`PERSONA_PLACES` in [`script_gaps.py`](../tools/parity/script_gaps.py)): `navigator`'s `userAgent`, `appVersion`, `platform`, `language` and `languages`, `navigator.userAgentData`'s `brands`, `mobile` and `platform`, the high entropy values, and what follows the locale in Intl and `toLocaleString()` |
| `behaviour` | Any other value that a getter or the third step gave, an index of an object that a getter gave or what an array or a plain object that a getter gave holds, an object that the walk reached through a getter, and the reports |
| `shape` | Everything else: the own properties of interface objects, prototypes and the platform's objects, their flags, names, lengths and order, prototypes, tags, the outcome of a constructor without `new` and of the checks, and an object that the walk reached through own properties and prototypes alone |

An object, and what it holds as data, takes the kind of the nearest getter on the way that the walk reached it: `navigator.languages own 0` is the Persona's, because the getter `languages` of `navigator` gave the array, and `viewport.segments["0"]` is behaviour, because the getter `segments` gave the array that holds it.

### The Machine's values

A value at a place that the Normal Form hides compares as `<machine>` on both sides, so the comparison only sees where it is and whether a getter threw there, as the wire's checks see `sec-ch-ua-platform-version`. A Session draws a new Machine each time, so a check holds what the Page gave there to that Machine, from the record before its Normal Form, in [`script_machine.py`](../tools/parity/script_machine.py):

- At a place that a Page reads from its start message, such as `innerWidth` or the time zone of `Intl.DateTimeFormat`, it must be what Chrome gives for that Machine (`PAGE_READS`), which is what `tests/test_machine.py` holds to [the forms](capture.md#the-form-of-a-hidden-value-of-a-script-fingerprint) of real Chrome's values.
- At a place whose value has the renderer's noise on it, `rtt` and `downlink` of `navigator.connection`, and at one that a Page works out by Chrome's own code (`WORKED_OUT_IN_A_PAGE`), such as the heap limit or the strings of a date, it must have the form that real Chrome's values have there.

In a frame or a worker, the check holds only the places of the machine there to the Machine: a frame's `outerWidth` is the Machine's window, and its `innerWidth` its element's, which the Normal Form keeps and compares.

A value that misses is a gap of the kind `machine`, in the field of its place, whose values say only `<machine>: the Machine's` and `<machine>: not the Machine's`, or the form and `<machine>: not of that form`, so that no drawn value reaches the list.

### Their known-gaps list

`tests/baselines/script_known_gaps.json` holds the Script Fingerprint's Known Gaps, beside the wire's, one per line, with the keys `release`, `case`, `kind`, `field`, `baseline` and `mitto`. A run fails on a gap that is not in the list and on one in the list that no longer exists, as the wire's does. There is one list for every Host, because a Script Fingerprint must not depend on the Host either. `grep '"kind": "shape"' tests/baselines/script_known_gaps.json` lists what is for mitto to match, with what an older Persona cannot turn off.

To write the list again from what the Page does now:

```bash
uv run python -m tools.parity --script
```

It needs a native library with the Page program beside it, prints how many gaps there are of each kind and for each Chrome Release, and takes `--baselines DIR` as the wire's command does. Read the diff before you commit it: a fixed gap only removes lines.

## What the native core states

Real Chrome gets its settings from preferences, field trials and the defaults in `//net`, and a source build has only the defaults ([ADR-0001](adr/0001-chromium-network-stack.md)). So the native core sets them itself, in code ported from Chrome with its names kept:

| File | States | Upstream |
|---|---|---|
| `native/network_session_configurator.cc` | HTTP/2 connection settings, what its connections do on a network change, and with `enable_http2` the ALPN list. QUIC parameters, and the origins that a field trial tells Chrome speak QUIC | `components/network_session_configurator`, and the hints in `net/http/http_server_properties.cc` |
| `native/ssl_config_service_mojo.cc` | TLS versions, cipher suites, key shares, ECH GREASE and trust anchors | `chrome/browser/ssl/ssl_config_service_manager.cc`, `services/network/ssl_config_service_mojo.cc` and `ssl_config_type_converter.cc` |
| `native/feature_list.cc` | Features that the ClientHello depends on, over TCP and QUIC, and those in `//net` that field trials turn on | The `--enable-features` and `--disable-features` lists in `chrome.txt` |
| `native/fetch_rules.cc`, `native/sec_header_helpers.cc` | [The headers that follow the page a request comes from](#the-initiator-and-each-hop), and what `//net` is told about that page | `SetFetchMetadataHeaders` and `MaybeRemoveSecHeaders` in `services/network/sec_header_helpers.cc`, `CorsURLLoader` and `URLLoader` in `services/network`, and the parts of the browser process and Blink that the section names |
| `native/client_hints.cc`, `native/fetch_rules.cc`, `native/http_header_map.cc`, `native/network_quality.cc`, `native/string_hasher.cc` | [Which client hints a request has](#client-hints), in what order, and the noise on `rtt` and `downlink` | `content/browser/client_hints`, `components/client_hints`, `services/network/public/cpp/client_hints.cc`, and Blink's `FrameFetchContext`, `HTTPHeaderMap`, `NetworkStateNotifier` and `StringHasher` |
| `native/mitto_core.cc` | That network state is partitioned and the HTTP cache split, in [the calls Chrome makes](#partitioning), and [where a cache lives](#the-http-cache) | `ChromeMainDelegate::CommonEarlyInitialization()`, `ChooseCacheType()` in `components/network_session_configurator` and `PopulateHttpCacheDirectories()` in `//content` |
| `native/stub_resolver_config_reader.cc` | The stub resolver: the built-in DNS client, additional query types, which is what asks for HTTPS records, secure mode automatic, and the upgrade to DNS over HTTPS | `ConfigureStubHostResolver` in `services/network/network_service.cc`, with the defaults in `chrome/browser/net/stub_resolver_config_reader.cc` |

A value stated here stays when a new **Engine Release** changes a default, so a Parity Check fails instead of mitto moving with the engine. That matters because an older **Chrome Release** must keep sending what it sent. All of this holds for every Persona, since the Chrome Releases of the Supported Window [send the same in it](persona.md#what-differs-between-chrome-releases).

When new Baselines list a feature in `chrome.txt` that the native core's build holds, add it to `native/feature_list.cc`. A feature outside that build, such as one in `//content`, does nothing in the native core. `chrome.txt` holds only names that look network-related, and a recording draws one group of each study, so a study with a small group may not show. The variations seed itself holds every study.

The trust anchors come from the root store compiled into `//net`, as they do in a Chrome that has just started. mitto has no component updates, so the list stays that of the Engine Release until the next one, whatever a Chrome that has run for a while has moved to ([what can differ](capture.md#what-can-differ-between-chrome-installations)).

`native/mitto_core.cc` creates one `NetworkChangeNotifier` and one `HostResolverManager` per process, as the network service does, and each **Session** has its own cache of answers on that manager. A Session starts DoH probes 5 seconds after the process created them, the default of `kDelayInitialDohProbeTimeoutParam`.

The notifier is also what makes a state of the HTTP/2 connections take effect: `spdy_go_away_on_ip_change`, which `native/network_session_configurator.cc` sets to true. Chrome takes the default of `//net` for it, true on Windows and false on macOS and Linux, and every **Persona** is Windows Chrome, so it would otherwise follow the **Host**. With true, a change of the IP address makes a Session's HTTP/2 connections go away, so that the requests on them can finish and new requests open another connection. With false they would close and fail those requests with `ERR_NETWORK_CHANGED`.

The first Session in a process waits up to a second for the system's DNS settings, which Chrome does not do, because Chrome has them long before a page asks for a host. A program's first request can come right away, and without those settings it would go to the system's resolver, which sees no HTTPS records.

No Parity Check can see any of this. The capture server's names are never looked up, because a `.localhost` name resolves to loopback by itself and a `.test` name goes through `--host-resolver-rules`, so a Baseline's ClientHello always holds ECH GREASE. `tests/test_dns.py` checks real ECH and an `h3` HTTPS record against a DNS server on loopback, which a Session reaches through the `dns_nameserver` setting. Only `mitto._native` offers that setting, and the public Session takes it only through `_settings`. Such a Session has a `HostResolverManager` of its own, so that other Sessions keep the system's settings.

### Real ECH

A site whose HTTPS record offers ECH gets an outer ClientHello at its public name, with the ClientHelloInner encrypted to its key. Chrome 155, 156 and 157 and a Session were recorded that way by hand ([how, and what came out](capture.md#real-ech)), which settles what this table says:

| With real ECH | Compared | How |
|---|---|---|
| The outer ClientHello | Yes | Its normal form is that of GREASE, which every Baseline holds |
| What comes after the handshake | Yes | It does not depend on ECH, and was byte for byte the Baselines with ECH accepted |
| That a refused ECH connects again with GREASE over TCP, and not over QUIC | In `tests/test_dns.py` | Against the capture server, which refuses it |
| The ClientHelloInner | In `tests/test_dns.py`, against a rule and not a Baseline | It is the outer ClientHello's offer for TLS 1.3 alone, as Chrome sent it, over TCP and QUIC. The test reads it with the key it made for the record, through `tests/local_ech.py` |
| The configuration ID, the encapsulated key and the payload | No | They are random or come from the record, and the normal form drops them. Their sizes were the same in Chrome and a Session |
| A server's `retry_configs`, another configuration, a HelloRetryRequest | No | The capture server offers none of these |

No Baseline holds a ClientHelloInner, and there is no **Recording Case** for one, for the reasons in [capture.md](capture.md#why-no-recording-case). So a Chrome Release that changed its ClientHelloInner and kept its outer ClientHello would show nowhere until someone records Chrome again.

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
| `sec-ch-*` staying on a subresource's hop | `FrameFetchContext::AddClientHintsIfNecessary` runs when a request starts, and not on a redirect. `FindClientHintsToRemove` then takes away what the page's permissions policy does not allow the next URL | Blink |
| `Referer` of the first hop | `SecurityPolicy::GenerateReferrer`, which `URLRequestJob::ComputeReferrerForPolicy` answers the same way | Blink, and `//net` from the second hop |
| `IsUrlPotentiallyTrustworthy` | Already in `//net` at this Engine Release, so it needs no port | `net/base/is_potentially_trustworthy.cc` |
| A frame's navigation, the `iframe` kind | `ComputeIsolationInfoForNavigation()` gives it its parent's site for cookies and an `IsolationInfo` of a frame, `ClientHintsExtendedData` its parent's hints, and only the outermost main frame parses `Accept-CH` or makes a `CriticalClientHintsThrottle` | `content/browser/renderer_host/render_frame_host_impl.cc`, `content/browser/client_hints/client_hints.cc`, `content/browser/loader/url_loader_throttles.cc` |
| Mode `same-origin` | `CorsURLLoader` fails the request with `kDisallowedByMode` once its CORS flag is set, at the start or at a redirect | `services/network/cors/cors_url_loader.cc` |
| `Origin` on an element in cors mode | `FetchParameters::SetCrossOriginAccessControl()` puts it in Blink's map, and `CorsURLLoader` writes over it only where it needs one | Blink, and `services/network/cors/cors_url_loader.cc` |

So the rules of `content`, `components` and Blink are what comes from outside `//net` and `services/network`. `Sec-Fetch-Frame-Top` and `Sec-Fetch-Frame-Ancestors` are behind features that are off by default and in the Baselines, so they are not ported. The parent of an `iframe` counts as top-level, as the page of every other kind does.

A **Persona**'s lines arrive at the native core with the rest of the headers, each in its place, along with what `network::ResourceRequest` would say: `mode`, `destination`, `credentials_mode`, `has_user_activation` and `initiator`. `fixed_headers` names what a caller gave or removed, which no rule touches. A request with none of this, which only `mitto._native` can make, sends what its headers say.

## A CORS preflight

`native/cors_preflight.cc` ports what Chrome's network service does for a request in cors mode, with the upstream names kept: `NeedsPreflight()` and `CheckPreflightRequired()` from `services/network/cors/cors_url_loader.cc`, `CreatePreflightRequest()` and `PreflightLoader` from `preflight_controller.cc`, `PreflightResult` and `PreflightCache` from `preflight_result.cc` and `preflight_cache.cc`, `CorsUnsafeNotForbiddenRequestHeaderNames()` from `cors_util.cc`, and `IsCorsSafelistedHeader()` and `CheckAccess()` from `services/network/public/cpp/cors/cors.cc`. The features they read are at their defaults: `kCorsPreflightCacheKeyTaintedOrigin` on, so a tainted request neither reads the cache nor adds to it, `kCorsNonWildcardRequestHeadersSupport` off, and a network context's `acam_preflight_spec_conformant` true. `RequestJob` holds a request back until its preflight has answered, at its start and at a redirect, as `CorsURLLoader` does with `kAvoidCorsURLLoaderRestartOnRedirect` on by default, and the cache is the Session's, as a network context's is.

### What stays different

- Chrome reports a failed preflight to DevTools and the console, which a Session has not.

## Client hints

What a server asks for ([the rules](persona.md#client-hints)) lives in a `ClientHintsStore` in each **Session**, on the network thread, so that the native core can read it on each hop with nothing to ask Python. All of this is outside the source export and is ported with its names kept:

| Rule | In Chrome | Where |
| --- | --- | --- |
| Which names are hints, and what `Accept-CH` holds | `GetClientHintToNameMap` and `ParseClientHintsHeader` | `services/network/public/cpp/client_hints.cc` |
| `Accept-CH` and `Critical-CH` count for nothing beside `Clear-Site-Data` | `PopulateParsedHeaders` | `services/network/public/cpp/parsed_headers.cc` |
| What goes without asking | `IsClientHintSentByDefault` | `third_party/blink/common/client_hints` |
| Keeping what an origin asked for | `PersistClientHints` and `GetAllowedClientHintsFromSource` | `components/client_hints/browser/client_hints.cc` |
| Taking it back | `ClearClientHintCacheAndReply`, from `SiteDataClearer` | `content/browser/browsing_data/clear_site_data_utils.cc` |
| A navigation's response, and each redirect on the way | `ParseAndPersistAcceptCHForNavigation`, from `CommitNavigation` and `OnRequestRedirected` | `content/browser/client_hints/client_hints.cc`, `content/browser/renderer_host/navigation_request.cc` |
| A navigation's hints | `AddNavigationRequestClientHintsHeaders` | `content/browser/client_hints/client_hints.cc` |
| A subresource's hints | `AddClientHintsIfNecessary`, with each origin's default permissions policy | Blink's `FrameFetchContext`, `permissions_policy_features.json5` |
| [The noise on `rtt` and `downlink`](persona.md#the-noise-on-rtt-and-downlink) | `GetRandomMultiplier`, `RoundRtt` and `RoundKbpsToMbps` of a navigation, and `RoundMbps` of a subresource | `content/browser/client_hints/client_hints.cc`, Blink's `network_state_notifier.cc` |
| `Critical-CH` | `CriticalClientHintsThrottle`, whose restart `RestartWithURLResetNow` makes a redirect to the first URL | `content/browser/client_hints`, `third_party/blink/common/loader` |
| `ACCEPT_CH` in ALPS | `AcceptCHFrameInterceptor` and `OnAcceptCHFrameReceived`. `//net` hands the frame to `OnConnected` in `TransportInfo`, from `GetAcceptChViaAlps()` | `services/network`, `content/browser/loader/navigation_url_loader_impl.cc` |

### Starting over

A restart for `Critical-CH` or for the frame in ALPS is a new `URLRequest` in the place of the old one, as the new loader is in Chrome, with the method and body that the navigation has come to. A `Referer` that has shrunk stays that way. Neither comes with an event for a caller, as in Chrome, where a page sees neither. A stream cannot go twice, so a request with one goes on without restarting.

A restart for the frame happens in `OnConnected`, when the request has sent nothing, so a server sees only the request with the hints. It stays at this hop, and carries on the chain, so `Sec-Fetch-Site` still knows where the navigation has been. After 20 of them, which a server can only cause by presenting another ALPS on each connection, the request ends with `ERR_TOO_MANY_ACCEPT_CH_RESTARTS`.

A restart for `Critical-CH` follows a whole request, so a server sees two. Chrome only has `CriticalClientHintsThrottle` for a navigation that starts with a safe method, so a `POST` never does this. It goes back to the first URL, and starts a new chain, so `Sec-Fetch-Site` there knows nothing of a hop on the way.

The hop after that restart has a quirk, which a Session copies. `ThrottlingURLLoader::FollowRedirect()` goes on from the restart with what that said still in `headers_update_params_`, which it only clears after the next real redirect. So on that hop the hints of the restart come first, and what the next origin should have follows, which sends those hints even to an origin that did not ask for them. Chrome 156 on macOS sent `sec-ch-ua-model` that way on 2026-11-07, to an origin that had only asked for `sec-ch-ua-arch`, and on the hop after that it did not. No Baseline holds this, because `client_hints_critical` has no redirect.

The capture server cannot say anything in ALPS, because Python's `ssl` cannot negotiate it, so no **Baseline** holds the order of a restart for the frame. [A check by hand over HTTP/3](capture.md#alps-over-http3) found a Session's order to be Chrome's, for one hint. The rule comes from the source: `resource_request_->headers.MergeFrom()` keeps each line it has, and adds the others at the end, before the network service adds Fetch Metadata. `tests/test_client_hints.py` goes through the rules with `accept_ch_via_alps`, a setting only tests can reach, in the place of what `//net` read.

Google's servers do send the frame, so this was also checked by hand on 2026-11-07 against `www.google.com`, on macOS. In a net log of Chrome 156 going there, the first `URLRequest` ends at `URL_REQUEST_DELEGATE_CONNECTED` with nothing sent, and the second has the new hints after `accept` and before `sec-fetch-site`, with the three that go by default still ahead of `upgrade-insecure-requests`. A Session restarted in the same way there, with the frame that `//net` read, and the same order. That Chrome also adds [lines for Google's domains](#googles-domains), which a Session does not. No test does this, because it takes the network.

### The order of a subresource's lines

Over the network, a script, image or fetch shows the lines that Blink holds in `ResourceRequest` in the order of its `HTTPHeaderMap`, which is a `WTF::HashMap` that Blink goes through from the first bucket to the last. So the order depends on each name's hash and on the size of the table, which grows with what is in it. It looks random, and it changes with what a server asks for: asking for one more hint can swap two others.

`native/http_header_map.cc` ports that table, with the hash in `native/string_hasher.cc`: `rapidhash` through `CaseFoldingHashReader`, `HashString24`, the probing of `HashTable::insert`, its growth at half full and `Rehash`, which goes through the old table in its own order. What goes in, in order, is `Content-Type`, the hints in the order of `AddClientHintsIfNecessary`, and `User-Agent` from `PrepareRequest`. `Accept` and the rest come later and from elsewhere, so they follow.

Before any of this was written, a script of this same model in Python gave the order of all 40 subresources that Chrome 156 on macOS sent for 8 sets of hints, from none to all of them. The Baselines of `client_hints_*` hold six sets from Windows Chrome, with and without a body, which reach all three sizes the table gets to. `tests/test_client_hints.py` holds two more sets. With no hint added, the native core leaves the Persona's order alone.

### What stays different

| | Chrome | A Session |
| --- | --- | --- |
| How long it keeps what an origin asked for | Until the user clears it, in its profile | As long as the Session lasts. `cookie_file` does not hold it |
| Delegating hints to another origin | `Permissions-Policy` and `Delegate-CH` from the page | A page has no response that a request can go back to, so a subresource to another origin only gets what goes by default |
| `<meta http-equiv="accept-ch">` | Read from the HTML | There is no HTML |
| `rtt`, `downlink` | Measured, with noise for each host | The estimate of the Session's Machine, which does not change, with Chrome's noise for each host |
| Viewport sizes | Follow the window | The Persona's values |
| `width`, `sec-ch-width` | From the layout of an image with a width | Not sent |
| `Clear-Site-Data` | Also clears cookies and the cache | Only takes back what an origin asked for |

## Partitioning

Chrome partitions connections, TLS sessions, DNS results and more by the top-level site, and splits its HTTP cache the same way, because `ChromeMainDelegate::CommonEarlyInitialization()` calls `HttpCache::SplitCacheFeatureEnableByDefault()` and `NetworkAnonymizationKey::PartitionByDefault()` ([what was measured](baselines.md#features-on-in-chromes-own-code)). `NetworkCore` makes the same two calls in the same order, right after the feature list exists and before anything can ask whether either is on or any cache exists.

Each request has an `IsolationInfo`, as in Chrome: a navigation's follows it as a top frame on each hop, and a subresource's stays that of its page. So over each protocol, a subresource from one top-level site does not share a connection with one from another, and a navigation shares one with what its own page asks of its own site. A cross-origin `fetch()` has no credentials, so `//net` gives it a connection apart from one that has them.

No normal form can show this, so `tests/test_initiator.py` counts the connections at the server, over HTTP/1.1, HTTP/2 and HTTP/3, and `tests/test_cache.py` counts the requests that a cached response saves. `Partitioned` cookies go in the partition of the same site.

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

`session.cookies.save(path)` writes what the C API lists to a JSON file, in the Python layer, with the Session's **Machine** beside the cookies. That keeps SQLite out of the build. The file has mode 0600 whatever the umask, and on Windows an access list of its own, which lets its owner alone in and takes nothing from its folder. The values are not encrypted. A new file takes the old one's place by a rename, so a crash leaves one or the other whole. `Session(cookie_file=path)` loads when it opens and saves when it closes, so a crash loses what came after the last save. A file that holds anything else stops the Session from opening, because closing it would otherwise overwrite that file.

The native core lists the cookies alone, as `{"version": 1, "cookies": [...]}`, and refuses what it does not know ([ADR-0003](adr/0003-c-abi-and-ctypes.md)). So the file is of version 2, `{"version": 2, "machine": {...}, "cookies": [...]}`, and the Python layer takes the Machine out and hands the native core the rest as version 1. The Machine keeps each of its fields and its seed, so that a later mitto that knows more fields can draw them from the seed and keep the others. A file of version 1, from mitto 0.1, holds no Machine and loads as `Machine.named("mitto-0.1")`, which every Session of mitto 0.1 claimed, and the next save writes version 2.

A Session that opens on a file claims its Machine, since a server that sees the same cookies from another machine can tell. A `machine=` that differs stops the Session from opening, and `Cookies.load()` of a file whose Machine is not the Session's adds nothing. Both raise a `ValueError`. Taking cookies to another Machine is up to a caller, cookie by cookie, with `Cookies.set()`.

Session cookies are saved too, as a browser does that restores its last session. A cookie that has expired by the time it loads stays out, because the store removes it on the next read.

### What stays different

| | Chrome | A Session |
| --- | --- | --- |
| A `Secure` cookie on `http://localhost` | `CookieAccessDelegateImpl`, in `services/network`, treats a potentially trustworthy URL as secure, so it can set one and receive it | `//net` with no delegate needs HTTPS |
| A page in a frame | Its `site_for_cookies` can be null, and its partition holds its ancestors | A page always counts as top-level |
| Settings that block a cookie, and First-Party Sets | `CookieSettings` and `FirstPartySetsManager`, in `services/network` | None, which is what a Chrome with no such settings does for a first-party cookie |
| When the store is written | Every 30 seconds, by `SQLitePersistentCookieStore` | When a caller saves, or at close |

## The HTTP cache

`Session(cache="memory")` or `Session(cache=path)` gives a **Session** the `HttpCache` of `//net`, through `URLRequestContextBuilder::EnableHttpCache()`, so `//net` decides what to keep, when to revalidate and what that sends, as it does in Chrome. The default is no cache. `Response.from_cache` is `URLRequest::was_cached()`, as `was_fetched_via_cache` is in Chrome: it says where the body came from, so a 304 makes it true although a request went out.

The `revalidation` **Recording Case** shows that `If-None-Match` reaches the wire in the place Chrome puts it, over each protocol. `tests/test_cache.py` checks what no **Baseline** holds: a response that saves a request, a revalidation, a directory that outlives its Session and its process, a `POST` that drops what the cache holds, a 206 response that runs past its range, and which requests share a response.

### Which backend

`ChooseCacheType()` picks a different backend per system, and that shows on the wire. Over HTTP/2 a 304's `HEADERS` frame usually holds `END_STREAM`, and `//net` tells `HttpCache` about the headers before it handles that. `HttpCache` updates the entry and then lets the network transaction go. If the write has already finished, that cancels a stream that is about to end, with an `RST_STREAM` that a server can see:

| | Backend | Writing an entry | `RST_STREAM` after a 304 |
| --- | --- | --- | --- |
| Chrome on Windows, and `Session(cache=path)` on every **Host** | Blockfile | Never finishes at once | None, as the **Baselines** show |
| Chrome on macOS and Linux | Simple | Can finish at once | Sent. Measured on macOS, Chrome 156 |
| Chrome in Incognito, and `Session(cache="memory")` | Memory | Finishes at once | Sent. Measured on macOS, Chrome 156 |

Every **Persona** is Windows Chrome, so a directory gets blockfile whatever the **Host** is, and that is what the replay of `revalidation` uses. Blockfile's destructor also waits for its work, so a process can end as soon as `close()` returns.

A response can also get there first, in which case the stream has already ended and even the memory backend cancels nothing, so that row says what usually happens.

### Where it lives

A directory holds what Chrome's `http_cache_directory` does, with `//net`'s files in its subdirectory `Cache_Data`, as `PopulateHttpCacheDirectories()` lays it out. This matters more here than in Chrome. A cache that `//net` cannot read makes it rename the directory it was given and delete it, and a caller can name any directory, so `//net` never gets one that a caller's files are in.

One Session at a time can have a directory. `//net` allows one backend for each path, and a second one in the same process waits for the first to go, so its requests would never end. Across processes Chrome counts on being one process, and a second one would overwrite the first's files. So the native core holds a `flock()` on the directory itself, which refuses a Session in the same process too, whatever name it comes by, and which goes away with the process however that ends. A Session that cannot have the directory fails to open with an `OSError`, and so does one that `//net` cannot make a cache for, because `//net` itself would go on with no cache and tell nobody.

The sizes are those of `//net` with no setting: at most 50 MiB in memory, and on disk from 80 MiB to 1% of the free space. A Session does not offer a size.

### What stays different

| | Chrome | A Session |
| --- | --- | --- |
| The body of a `POST` | A form's has an identifier, so its response is kept for going back to it | Has none, so a `POST` always passes through the cache. It still drops what the cache holds for that URL when it succeeds |
| `No-Vary-Search` | Its entries go to disk in `No_Vary_Search`, by writes that go on after `HttpCache` has gone | Last as long as the Session, so that nothing touches the directory after `close()`. A later Session finds a response by its exact URL |
| A wait for another request's entry | Has no timer around it | Counts towards `connect` of a `Timeout`, because no connection is ready yet |

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

A proxy's URL may hold a password, so no message and no `repr` repeats it. A Session does not keep the URL in Python.

### Credentials over SOCKS5

`SOCKS5ClientSocket` in `//net` offers only "no authentication", so real Chrome cannot use a SOCKS5 proxy that needs credentials. A Session whose `socks5` URL holds some does the username and password of [RFC 1929](https://www.rfc-editor.org/rfc/rfc1929) below `//net`, in `native/socks5_auth_socket_factory.cc`, with no patch and no local port. Only such a Session has this code: every other one has nothing between `//net` and its sockets.

Each TCP socket of that Session is a wrapper. Nothing tells a `ClientSocketFactory` which socket goes to the proxy, because it only sees an address, which a connection around the proxy may share, and a NetLog source, which is the same for a tunnel of HTTPS and for HTTPS with no proxy. So the first write decides. A Session has one proxy and only `SOCKS5ClientSocket` starts a connection with its three bytes of greeting, which neither TLS nor HTTP can do, so credentials reach no other peer. Any other first write makes the wrapper pass everything on.

On that greeting, the wrapper has its own exchange with the proxy in `native/socks5_auth_exchange.cc`, and then answers that the proxy needs nothing, as `SOCKS5ClientSocket` expects. That one goes on to send the name, so the proxy finds it as it does with no credentials. What follows inside the tunnel does not change, so a target has the same normal form as through any other proxy. A test holds that against the Baseline of `proxy_navigation`.

This runs from the write and not from `Connect()`, because `SOCKSConnectJob` turns any error of `Connect()` into `ERR_PROXY_CONNECTION_FAILED`, and tries the proxy's next address. From here an error stays what it is, and the job's 30 seconds for the SOCKS5 handshake cover the exchange.

The greeting offers "no authentication" and "username and password". A proxy that picks the first never gets the credentials, as an HTTP proxy that does not ask never does. SOCKS5 has nothing to remember, so each new connection to the proxy gives them again.

| What the proxy answers | The request ends with |
|---|---|
| A status other than success | `ERR_PROXY_AUTH_REQUESTED`, as from an HTTP proxy that refuses |
| Neither of the methods offered | `ERR_PROXY_AUTH_UNSUPPORTED` |
| Another version, or it closes the connection during the exchange | `ERR_SOCKS_CONNECTION_FAILED`, as in `SOCKS5ClientSocket` |

These are all a `ProxyError`. A read or a write that fails during the exchange keeps its own error, as in `SOCKS5ClientSocket`, so a reset connection is a `ConnectionFailedError`. A proxy that asks a Session with no credentials sees the greeting of `//net`, and ends the request with `ERR_SOCKS_CONNECTION_FAILED`, as it does in Chrome.

RFC 1929 sends bytes and names no encoding, so a percent-escape in the URL stands for its own byte, and `%FF` is a byte that is not text. HTTP's credentials go through `GetIdentityFromURL()`, which keeps such an escape as text. RFC 1929 has one byte for each length, so anything over 255 bytes once decoded is refused when the Session is created. It lists 1 as the shortest, but a URL with only a username or only a password sends the other with a length of 0 and leaves the choice to the proxy.

## Redirects

`//net` decides what a redirect does, as it does in Chrome: the limit of 20, the method of the next hop and when a body goes. The native core defers each redirect in `OnReceivedRedirect()` and continues it with `FollowDeferredRedirect()`, as `URLLoader` in Chrome's network service does.

It continues from a task of its own, and that shows on the wire. Over HTTP/2 a redirect's `HEADERS` frame usually holds `END_STREAM`, and `//net` tells its delegate about the headers before it handles that. Following the redirect from inside that call would cancel a stream that is about to end, with an `RST_STREAM` that a server can see. Chrome's answer to a redirect comes back from another process, so it sends none.

On each hop it first changes the headers that follow the page a request comes from ([which](#the-initiator-and-each-hop)).

A caller's `Authorization` does not go to another origin, and stays gone if a later hop comes back. This is step 13 of [HTTP-redirect fetch](https://fetch.spec.whatwg.org/#http-redirect-fetch), which `ResourceLoader` does in Blink, so the native core does it in the same place, as a header to remove when it continues. A caller's `Cookie` goes the same way. Fetch does not name it, because a page cannot send one, but it has no domain to keep it where it belongs. The cookies that `//net` adds from a **Session**'s store are not touched, because `//net` decides those on each hop.

`follow_redirects=False` makes a redirect the response, also from a task of its own, so the stream ends as it does when a redirect is followed. Its content is empty because `//net` does not read the body of a redirect, as with `redirect: "manual"` in Fetch. A redirect to a URL that is not valid still ends the request with `ERR_INVALID_REDIRECT`, because `//net` checks that before it tells its delegate.

## Streams

`Session.stream()` gives a response's body as the caller reads it, and `data=` takes a file object or an iterable of bytes as a request's body. Neither holds more of a body than a few buffers, so a body of any size takes the same memory, and `tests/test_big_transfers.py` moves 1 GiB each way to show it. A reader that stops makes the server wait through flow control, and so does a server that stops reading a body. Leaving the `with` statement ends the request.

A request's stream is a subclass of `net::UploadDataStream` of mitto's own, `BodyStream`, written like `ChunkedDataPipeUploadDataStream`, which is what Chrome's network service makes of a page's `ReadableStream`. `net::ChunkedUploadDataStream` will not do: it keeps every byte until the request ends, and it lets a stream go over HTTP/1.1.

The caller's file or iterable is read in the thread that waits for the response, and only when `//net` wants more. That has three results: a request that fails before `//net` can use a body has read none of it, an error the iterable raises reaches the caller and ends the request so that the server does not take what went for the whole body, and nothing on the network thread ever waits for Python.

| The body | What it sends | Over HTTP/1.1 | Which Chrome does the same |
|---|---|---|---|
| An iterable, or a file that cannot seek | No `Content-Length`, over HTTP/2 or HTTP/3 | Refused, with `ERR_ALPN_NEGOTIATION_FAILED` and nothing on the wire | `fetch()` with a `ReadableStream`, which the Baselines of `fetch_post_stream` hold |
| A file that can seek, or an iterable with a `Content-Length` in `headers` | `Content-Length`, in the place `//net` gives it on any body | Sent | `fetch()` with a `File`, which no Baseline holds. `//net` writes the header from the size of the body, whatever kind of body it is, so nothing here decides its place |

A refusal raises a `TLSError`, which is a `RequestError`, named `ERR_ALPN_NEGOTIATION_FAILED`. That net error has that type everywhere, so it has it over plain HTTP too, where no TLS is involved.

A stream of a size that does not keep to it fails. An iterable that gives more or less than its `Content-Length` raises a `ValueError`, and a file that turns out shorter than it was fails with `ERR_UPLOAD_FILE_CHANGED`, as `//net` says of a `File`. One that has grown goes no further than its size, also as for a `File`.

Over HTTP/2 the end of a stream of unknown size is a `DATA` frame of its own with nothing in it, and over HTTP/3 the end of the QUIC stream. The Baselines show it, so a part never goes in the same read as the end. The body comes in parts as they come, each in a frame, so one page's stream of one part makes the same frames as a replay's.

`//net` reads no response until the whole body has gone or the server has ended the stream, so a response does not arrive during a body, as it does not in Chrome, where a page has to ask for `duplex: "half"`. The caller's iterable has to end for the response to come, except for a `Session.stream()` of a server that ends it.

A wait for the caller's next part is not a wait for the server, so the `read` timer stops during it, and the `total` timer does not. A file or iterable that blocks holds up the thread that reads it, so a timer's error waits until it gives the next part.

### A stream goes once

Fetch fails a request with a body that has no source on any redirect but a 303, in [step 11 of HTTP-redirect fetch](https://fetch.spec.whatwg.org/#http-redirect-fetch), which Chrome does in `CorsURLLoader`. A Session does the same with `ERR_UPLOAD_STREAM_REWIND_NOT_SUPPORTED`. A 303 drops the body as it does in Chrome, so later redirects of that request have nothing to send again, and `follow_redirects=False` makes any redirect the response. A 421 is the response too, where `//net` would send any other body again, because Chrome tells it that the body has no source.

`//net` goes on a new connection when a reused one turns out to be gone, and sends the body again. So `BodyStream` keeps the first 64 KiB of a stream, as Chrome keeps the same amount of a page's. A stream that has gone further fails with the same error, where Chrome's says `ERR_FAILED`.

### What stays different

| | Chrome | A Session |
|---|---|---|
| The error when a stream cannot go again | `ERR_INVALID_ARGUMENT` for a redirect and `ERR_FAILED` for another attempt, which a page sees only as `TypeError: Failed to fetch` | `ERR_UPLOAD_STREAM_REWIND_NOT_SUPPORTED`, which says what happened. A caller who sends a bad argument gets `ERR_INVALID_ARGUMENT` |
| A `File` on a 307 or another attempt | `//net` reads the file again | Any stream goes once, so a file fails as an iterable does. A caller who needs that sends bytes |

## Certificate verification

No Parity Check can see how a certificate is verified, because the verdict is not on the wire. A **Session** uses the verifier in `//net` that Chrome uses, with the Chrome Root Store, which at Chrome 157 is the only choice on Windows, macOS and Linux (`chrome_root_store_only`). `native/mitto_core.cc` wraps it in the same `CoalescingCertVerifier` and `CachingCertVerifier` that `NetworkContext` does.

A chain that arrives without an intermediate gets it from the URL in the certificate's Authority Information Access extension. Chrome's fetcher, `CertNetFetcherURLLoader`, lives in `services/cert_verifier`, outside the source export, so a Session has `CertNetFetcherURLRequest` from `//net`. They are written alike: HTTP only, no credentials and no secure DNS, 15 seconds and 64 KiB for an intermediate, and a request on the same context as the page's. A Session's fetcher shuts down with it, which ends a fetch that a worker thread is waiting for.

`Session(additional_trust_anchors=[pem, ...])` adds roots beside the Chrome Root Store, through `CertVerifyProc::InstanceParams`, which is how Chrome receives its own additional certificates. Each entry is the text of a PEM file. A self-signed server certificate can be its own anchor, and an anchor's own dates and constraints are not enforced. This is what mitto offers in place of the roots that Linux users add to the system, which it does not read ([ADR-0006](adr/0006-linux-library-without-system-libraries.md)). On macOS the verifier also reads the roots a user has added to the Keychain, as Chrome does.

On Windows the verifier also reads Windows' certificate stores, as Chrome on Windows does, with no change to `//net` (`net/cert/internal/trust_store_win.cc`). It takes what a user, an administrator or group policy has added there, for the computer and for the user, and leaves out the roots that come with Windows, so the Chrome Root Store still decides about every public server:

| Store | What its certificates are to the verifier |
| --- | --- |
| Trusted Root Certification Authorities | Roots, whose own dates and constraints are enforced, unlike those of `additional_trust_anchors`. A self-signed server certificate there is trusted as it is |
| Intermediate Certification Authorities | Intermediates that a chain can be built through |
| Trusted People | Self-signed server certificates that are trusted as they are |
| Untrusted Certificates | Certificates that no chain may use, whatever trusts them |

A certificate whose properties in the store leave out server authentication counts for nothing. A process reads the stores once, at its first verification, so a certificate added afterwards counts from the next process on.

`//net` refuses QUIC to a root that is not known, which means one outside the Chrome Root Store, with `ERR_QUIC_CERT_ROOT_NOT_KNOWN`. So a server whose chain ends at an added root stays on HTTP/1.1 or HTTP/2, whatever its `Alt-Svc` says, as it does in Chrome. Only `origins_to_force_quic_on` lifts that, which is what a Parity Check of `h3` uses, and the public Session does not offer it.

### What stays different

| | Chrome | A Session |
|---|---|---|
| Certificate Transparency | `MultiLogCTVerifier` checks SCTs against the logs in `ct_known_logs`, and `ChromeCTPolicyEnforcer` refuses a chain to a known root that has too few, with `ERR_CERTIFICATE_TRANSPARENCY_REQUIRED` | `DoNothingCTVerifier` and `DefaultCTPolicyEnforcer`. The enforcer and the log list live in `components/certificate_transparency`, outside the source export, so a Session accepts a certificate that Chrome would refuse for missing SCTs |
| Revocation | A CRLSet that a component keeps up to date | `CRLSet::BuiltinCRLSet()`, the fixed list compiled into `//net` |
| Root store | The compiled-in list, until a component brings a newer one | The compiled-in list of the **Engine Release** |

These let a Session accept what Chrome would refuse. None of them changes what a server sees: the ClientHello still offers SCTs, and it does not depend on `additional_trust_anchors`. A fetch for a certificate sends the same lines as Chrome 156 on macOS, in the same order: `Host`, `Connection`, `User-Agent`, `Accept-Encoding` and `Accept-Language`. Its `User-Agent` is the **Persona**'s, as Chrome's is its own.

## Google's domains

Real Chrome adds lines for Google's domains, and a Session sends none of them. [capture.md](capture.md#what-chrome-adds-for-googles-domains) has which domains and requests get them, where they go, and what was measured apart from what was read in the source.

These are **Known Gaps**, but `tests/baselines/known_gaps.json` has no line for them. No **Baseline** holds them, because the capture server's names are not Google's, so no Parity Check can see them. This list is where they are written down:

| Line | Why a Session does not send it |
|---|---|
| `x-browser-validation` | It hashes a seed and an API key inside Chrome with Chrome's `User-Agent`. We do not take those out of Chrome, and a recorded value would still come from them |
| `x-sec-ch-ua-not-a-border` | A closed library inside Chrome makes it from the same seed and key, and it changes every five minutes |
| `x-client-data` | It follows the field trials a Chrome drew, and a Session has no seed |
| `x-chrome-id-consistency-request` | It holds Chrome's OAuth client ID, and it asks `accounts.google.com` for what only Chrome's account code can use |
| `x-browser-channel`, `x-browser-year`, `x-browser-copyright` | They are the same in a whole build, so they could be **Persona** data. But Chrome always sends them with `x-browser-validation`, so with only these a request would be one that no Chrome sends |

So a server at those domains that reads these lines can tell a Session from a real Chrome. Nothing here changes what a Session sends anywhere else.

## Tests of the comparison

Real Baselines are recorded by hand on a Windows machine, so `tests/parity` tests the comparison itself with a sample: the real normal forms of Chrome 156 on macOS in `tests/capture/data`. It changes that sample on purpose and expects the run to fail, both through the native core and, where there is no native library, through a stand-in for it. `tests/parity/test_replay.py` checks that each replay goes where its case went, and that a replay compared with itself has no gaps, so that what the list holds is about mitto and not about the replay.
