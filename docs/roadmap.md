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

- Trimmed source export per Engine Release. It is produced by hand on a Linux machine and published as a release asset, so that GitHub-hosted runners can build the native core within their disk and time limits.
- CI on GitHub-hosted Linux x86_64 and macOS arm64 runners, building from that export.
- Capture kit: a local server that records the raw TLS ClientHello, HTTP/2 frames, QUIC transport parameters and HTTP headers. It comes with a procedure for recording real Windows Chrome as **Baselines**, one per Chrome Release in the **Supported Window** and per main **Request Kind**. Baselines are recorded by hand on a Windows machine and committed as data.
- Network stack configuration that mirrors Chrome's network service: enabled features, HTTP/2 settings, QUIC, ALPS, encrypted ClientHello (ECH) GREASE and post-quantum key shares. Each item is checked against the Baselines.
- Windows **Persona** data for each Chrome Release:
  - user agent and client hints;
  - `Accept` and priority per Request Kind;
  - `Accept-Language`;
  - header order;
  - TLS, HTTP/2 and QUIC deltas for the older releases.
- **Parity Checks** that run offline against the capture server and gate CI.
- **Exit:** Parity Checks pass for all three Chrome Releases and the main Request Kinds, over HTTP/1.1, HTTP/2 and HTTP/3.

### M2: HTTP client features

- **Session** API:
  - methods, query parameters and headers;
  - bodies: form, JSON, multipart and streamed;
  - timeouts;
  - responses as bytes, text, JSON or a stream;
  - a typed error hierarchy.
- Cookies (optionally persisted), redirects, an optional HTTP cache, and proxies: HTTP, HTTPS and SOCKS5, with authentication.
- DNS and certificate verification as Chrome does them, including the Chrome Root Store.
- Headers that follow the **Initiator**: `Sec-Fetch-Site`, `Referer`, `Origin` and same-site cookie rules.
- An asyncio API that shares the same Session machinery.
- **Exit:** feature tests and Parity Checks pass, and the API reference is written.

### M3: Packaging and release

- Release builds on the same GitHub-hosted runners as CI.
- Wheels: `py3-none` for manylinux x86_64 (the glibc floor follows Chromium's Linux sysroot) and for macOS arm64. Symbols are stripped and shipped as a separate symbols archive. The target is about 5 MB per wheel.
- A release routine every two weeks, following Chrome's stable milestones: move the Engine Release to the new stable, refresh Persona data, re-record Baselines, rebuild and publish.
- **Exit:** `pip install mitto` works on both Hosts, and 0.1.0 is on PyPI.

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
