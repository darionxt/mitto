# Roadmap

Terms in **bold** are defined in [CONTEXT.md](../CONTEXT.md). Architectural decisions are in [docs/adr](adr/).

## Scope agreed so far

| Topic | Decision |
|---|---|
| First deliverable | The HTTP client. The **Runtime** is a later phase. |
| Hosts | Linux x86_64 and macOS arm64 |
| Persona | Windows Chrome |
| Supported Window | Current stable plus the two previous Chrome Releases ([ADR-0005](adr/0005-older-releases-as-data.md)) |
| Network | Chromium's own stack, built from source ([ADR-0001](adr/0001-chromium-network-stack.md), [ADR-0002](adr/0002-build-inside-chromium-tree.md)) |
| Python API | Its own API, familiar to people who know `requests`, with both sync and asyncio versions. The network stack owns HTTP semantics ([ADR-0004](adr/0004-network-stack-owns-http-semantics.md)). |
| Python boundary | C ABI and `ctypes`, one `py3-none` wheel per Host ([ADR-0003](adr/0003-c-abi-and-ctypes.md)) |
| Python versions | 3.12 and later |
| Licence and distribution | Apache-2.0, public on GitHub, published on PyPI as `mitto` |
| Build resources | CI runs only on GitHub-hosted runners. Anything outside CI, such as producing source exports and recording Baselines, is done by hand. |
| First Engine Release | Chrome 157, the stable release when the project started |

## Milestones

### M0: Walking skeleton

Goal: prove the whole pipeline end to end before investing in features.

- Workspace outside the repository, on a large disk. It holds depot_tools, a Chromium checkout pinned to the Engine Release tag, and the build output. One environment variable points the tooling at it.
- Native core skeleton: a GN target inside the Chromium tree that links `//net`, a versioned C header, and one exported call that performs a blocking GET.
- Python package skeleton: `src/` layout, a `ctypes` loader, ruff and pyright configuration, and pytest.
- **Exit:** a GET request from Python works through the native core on macOS arm64 and on Linux x86_64.

### M1: Wire parity

Goal: mitto's **Wire Fingerprint** matches real Chrome. This is the project's core value, so it comes before features.

- Trimmed source export per Engine Release. It is produced by hand on a Linux machine and published as a release asset, so that GitHub-hosted runners can build the native core within their disk and time limits. It includes the patches in `tools/workspace/patches` ([ADR-0006](adr/0006-linux-library-without-system-libraries.md)).
- CI on GitHub-hosted Linux x86_64 and macOS arm64 runners, building from that export.
- Capture kit: a local server that records the raw TLS ClientHello, HTTP/2 frames, QUIC transport parameters and HTTP headers. It comes with a procedure for recording real Windows Chrome as **Baselines**, one per Chrome Release in the **Supported Window** and per main **Request Kind**. Baselines are recorded by hand on a Windows machine and committed as data. See [capture.md](capture.md) and [baselines.md](baselines.md).
- Network stack configuration that mirrors Chrome's network service: enabled features, HTTP/2 settings, QUIC, ALPS, encrypted ClientHello (ECH) GREASE and post-quantum key shares. Each item is checked against the Baselines.
- Windows **Persona** data for each Chrome Release:
  - user agent and client hints;
  - `Accept` and priority per Request Kind;
  - `Accept-Language`;
  - header order;
  - TLS, HTTP/2 and QUIC deltas for the older releases. Chrome 155 and 156 turned out to have none: their [Personas](persona.md#what-differs-between-chrome-releases) differ from Chrome 157's in `sec-ch-ua` and `User-Agent` only, so a Persona carries no engine settings yet. Where a later Chrome Release differs, the same page says how.
- **Parity Checks** that run offline against the capture server and gate CI. See [parity.md](parity.md).
- **Exit:** Parity Checks pass for all three Chrome Releases and the main Request Kinds, over HTTP/1.1, HTTP/2 and HTTP/3.

### M2: HTTP client features

Goal: turn the native core into the library people use, with the HTTP behavior of Chrome's network stack, still checked against **Baselines**. [api.md](api.md) has every public name, and [from-requests.md](from-requests.md) has what changes for code that uses `requests`.

- **Session** API, sync and asyncio, on the same machinery ([ADR-0003](adr/0003-c-abi-and-ctypes.md)):
  - methods, query parameters and headers;
  - bodies: form, JSON, multipart and streamed;
  - timeouts;
  - responses as bytes, text, JSON or a stream;
  - a typed error hierarchy, sorted by what a caller does next.
- Cookies, optionally kept in a file that the Python layer writes, redirects and an optional HTTP cache.
- Proxies, as a setting of a Session: HTTP, HTTPS and SOCKS5, with authentication. Real Chrome cannot give credentials to a SOCKS5 proxy, so a Session does that below `//net`, with no patch ([ADR-0006](adr/0006-linux-library-without-system-libraries.md)).
- DNS as Chrome's stub resolver does it, which finds HTTPS records and so real ECH. Certificate verification as Chrome does it, including the Chrome Root Store. Also an option to add trust anchors, because on Linux the Chrome Root Store is otherwise the only source of trust ([ADR-0006](adr/0006-linux-library-without-system-libraries.md)).
- Headers that follow the **Initiator**: `Sec-Fetch-Site`, `Referer`, `Origin` and same-site cookie rules. Connections and the HTTP cache are [partitioned by top-level site](parity.md#partitioning), as in Chrome.
- Client hints that follow what each origin asks for, in `Accept-CH`, `Critical-CH` or the `ACCEPT_CH` frame in ALPS, with their values in Persona data. See [persona.md](persona.md#client-hints).
- The three differences that M1 left: `spdy_go_away_on_ip_change`, `sec-ch-ua*` and `Sec-Fetch-*` over plain HTTP, and `trust_anchors` in a long-running Chrome, which stays the list compiled into the **Engine Release** ([what can differ](capture.md#what-can-differ-between-chrome-installations)).
- A Session sends none of the lines Chrome adds for Google's domains. They need what only Chrome holds, or would make a set no Chrome sends. They are **Known Gaps** that no **Parity Check** can see, and [parity.md](parity.md#googles-domains) lists them.
- **Left out:** WebSocket, CORS preflights, HTTPS upgrades of navigations, an `auth=` argument for origins (an `Authorization` header works), `verify=False`, client certificates, Certificate Transparency enforcement, Reporting and NEL, shared dictionaries and preconnects.
- **Still open:** what a resumed connection sends and whether the ClientHello check is enough for ALPS over HTTP/3, and whether a Baseline can hold a handshake with real ECH. The answers go in [capture.md](capture.md).
- **Exit:** feature tests pass through both the sync and the asyncio API. Parity Checks pass with an empty known-gaps list for Chrome 155, 156 and 157 and every **Recording Case**, on both **Hosts**. The API reference is written. The **Supported Window** stays at those three and the Engine Release at 157 until then, because moving up is the routine of M3.

### M3: Packaging and release

- Release builds on the same GitHub-hosted runners as CI.
- Wheels: `py3-none` for manylinux x86_64 (the glibc floor follows Chromium's Linux sysroot) and for macOS arm64. Symbols are stripped and shipped as a separate symbols archive. The target is about 5 MB per wheel.
- A release routine every two weeks, following Chrome's stable milestones: move the Engine Release to the new stable, refresh Persona data, re-record Baselines, rebuild and publish.
- Yank the `0.0.0` placeholder when 0.1.0 ships. It only holds the name on PyPI: it has no code, and its wheel installs on any platform and Python version. If it stays, pip falls back to it wherever no 0.1.0 wheel fits, and the install succeeds with nothing to import.
- **Exit:** `pip install mitto` works on both Hosts, 0.1.0 is on PyPI, and the `0.0.0` placeholder is yanked.

### Later phase: Runtime

The headless JavaScript **Runtime** gets its own planning once M3 ships. The direction is V8 from the Engine Release, bindings generated from Chromium's Web IDL files, and a self-written DOM, CSS and layout. Its network requests go through the same Session machinery.

## Cross-cutting rules

- C++ follows Chromium's style guide and allowed-features list ([ADR-0002](adr/0002-build-inside-chromium-tree.md)). Python follows ruff with strict type checking.
- Security: sanitizer builds (ASan, UBSan) run in CI. Every parser we write ourselves gets a fuzz target.
- Large artifacts (checkouts, build output, caches) live in the workspace, never in the repository.

## Risks

- **Field trials.** Chrome enables some network behavior through server-side experiments that a source build does not see. Mitigation: Baselines always come from real Chrome, never from our own build.
- **Randomized handshakes.** Chrome randomizes TLS extension order and GREASE values, so Parity Checks compare normalized forms rather than raw bytes.
- **Build cost.** Chromium builds are slow on ordinary developer machines. Building only the native core's targets, never the browser, keeps iteration usable.
- **Hosted runner limits.** GitHub-hosted runners have little disk space and a six-hour job limit, and their minutes are free only for public repositories. The trimmed export and build caching must keep a CI build inside those limits.
- **Maintenance cadence.** Chrome ships a new stable milestone every two weeks, and falling behind makes the Persona stale. The routine, including its manual steps, has to be cheap and scripted.
