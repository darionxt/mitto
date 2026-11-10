# Roadmap

Terms in **bold** are defined in [CONTEXT.md](../CONTEXT.md). Architectural decisions are in [docs/adr](adr/).

## Scope agreed so far

| Topic | Decision |
|---|---|
| Phases | The HTTP client first, M0 to M5. Then the **Runtime**, from M6. |
| Hosts | Linux x86_64 and macOS arm64, and Windows x64 from M4 |
| Persona | Windows Chrome |
| Machine | Drawn for each **Session** from a weighted table, from M5 ([ADR-0007](adr/0007-a-machine-of-its-own-per-session.md)) |
| Supported Window | Current stable plus the two previous Chrome Releases ([ADR-0005](adr/0005-older-releases-as-data.md)) |
| Network | Chromium's own stack, built from source ([ADR-0001](adr/0001-chromium-network-stack.md), [ADR-0002](adr/0002-build-inside-chromium-tree.md)) |
| Python API | Its own API, familiar to people who know `requests`, with both sync and asyncio versions. The network stack owns HTTP semantics ([ADR-0004](adr/0004-network-stack-owns-http-semantics.md)). |
| Python boundary | C ABI and `ctypes`, one `py3-none` wheel per Host ([ADR-0003](adr/0003-c-abi-and-ctypes.md)), which also carries the Runtime ([ADR-0009](adr/0009-a-process-for-each-page.md)) |
| Runtime | V8 from the Engine Release, bindings generated from Chromium's Web IDL files, and a DOM, CSS and layout of its own ([ADR-0008](adr/0008-v8-and-bindings-from-web-idl.md)). Each **Page** runs in a process of its own and sends its requests through its Session ([ADR-0009](adr/0009-a-process-for-each-page.md)). |
| Python versions | 3.12 and later |
| Licence and distribution | Apache-2.0, public on GitHub, published on PyPI as `mitto`. From M5 the wheel also carries a data table under CC BY-SA 3.0 ([ADR-0007](adr/0007-a-machine-of-its-own-per-session.md)) |
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
- **Exit:** feature tests pass through both the sync and the asyncio API. Parity Checks pass with an empty known-gaps list for Chrome 155, 156 and 157 and every **Recording Case**, on both **Hosts**. The API reference is written. The **Supported Window** stays at those three and the Engine Release at 157 until then, because moving up is the routine of M3.

### M3: Packaging and release

- Release builds on the same GitHub-hosted runners as CI.
- Wheels: `py3-none` for manylinux x86_64 (the glibc floor follows Chromium's Linux sysroot) and for macOS arm64. Symbols are stripped and shipped as a separate symbols archive. The target is about 5 MB per wheel.
- A release routine every two weeks, following Chrome's stable milestones: move the Engine Release to the new stable, refresh Persona data, re-record Baselines, rebuild and publish. See [releasing.md](releasing.md).
- Yank the `0.0.0` placeholder when 0.1.0 ships. It only holds the name on PyPI: it has no code, and its wheel installs on any platform and Python version. If it stays, pip falls back to it wherever no 0.1.0 wheel fits, and the install succeeds with nothing to import.
- **Exit:** `pip install mitto` works on both Hosts, 0.1.0 is on PyPI, and the `0.0.0` placeholder is yanked.

### M4: Windows Host

Goal: mitto installs and runs on Windows x64 as well. The **Persona** stays Windows Chrome on every **Host**, so this adds a Host and changes nothing a Session presents.

- A workspace and a build of the native core on Windows, with Chromium's toolchain and the machine's Visual Studio and Windows SDK, as Chromium builds on Windows outside Google.
- A Windows slice in the source export, made by hand on a Windows machine, so that hosted Windows runners build from the export as the other Hosts' runners do.
- The asyncio API on Windows, whose default event loop cannot watch a file descriptor. A thread per Session waits in the native core there ([ADR-0003](adr/0003-c-abi-and-ctypes.md)).
- Certificate verification as Chrome on Windows does it, which also reads Windows' certificate stores.
- CI, a `py3-none-win_amd64` wheel and the release workflow on GitHub-hosted Windows runners.
- M4 holds back neither 0.1.0 nor the release routine. The first release after its exit ships the Windows wheel.
- **Exit:** `pip install mitto` works on Windows x64, and every test passes there from the installed wheel, Parity Checks included.

### M5: A Machine for each Session

Goal: each **Session** claims a **Machine** of its own instead of the one its **Persona** claimed in 0.1, so that Sessions no longer share one set of machine hints ([ADR-0007](adr/0007-a-machine-of-its-own-per-session.md)).

- A Machine drawn by default from a weighted table that ships with mitto, repeatable from a seed, with any value fixed by the caller. It is drawn the same way on every **Host** and never read from the machine mitto runs on.
- The Machine saved with the cookies, in a new version of the cookie file.
- Chrome's noise on `rtt` and `downlink`, for each host, in the native core.
- `sec-ch-ua-platform-version` hidden as `<machine>` in the normal form.
- A step of the release routine that refreshes the table.
- M5 runs beside M4 and holds back neither. It ships in a minor version, with release notes on what a Session sends now.
- **Exit:** a Session sends the hints of a drawn Machine, the same ones after its cookie file is loaded in another process, `rtt` and `downlink` differ from host to host as Chrome's do, and the Parity Checks pass.

### M6: Runtime skeleton

Goal: prove the **Runtime**'s whole pipeline on every Host before investing in breadth, as M0 did for the HTTP client.

- V8 from the Engine Release in the native build, with Chrome's settings, and Chrome's whole ICU data file ([ADR-0008](adr/0008-v8-and-bindings-from-web-idl.md)). The source export and its Windows slice carry what they need.
- The Page program, which the native core starts for each **Page**, and the channel between them ([ADR-0009](adr/0009-a-process-for-each-page.md)). M6 chooses the channel: Chromium's Mojo or one of the native core's own.
- A bindings generator that reads the Engine Release's Web IDL files and writes V8 bindings, with members in the order Chrome installs them. At first it covers what the skeleton needs, such as `Window`, `Document`, `Element` and `console`.
- A first HTML parser, which builds the tree of simple documents and runs their classic scripts, inline and external, in document order. It gets a fuzz target, as every parser mitto writes does.
- An event loop with tasks, microtasks and timers, on the real clock.
- A page's `fetch()`, `XMLHttpRequest` and scripts, loaded through its Session with the Request Kind and Initiator that the page gives them, and `document.cookie` on the Session's cookies.
- `Session.open_page()`, sync and asyncio, a Page's `evaluate()` and `close()`, and an error when its process ends. The API stays private until the Runtime ships.
- CI builds the Page program and runs its tests on every Host. The release workflow leaves it out of the wheel.
- **Exit:** on every Host, a Page that a Session opens loads a page whose script fetches JSON with the Session's cookies and writes it into the document, and Python reads the result through both the sync and the asyncio API. A Page whose process crashes raises an error, and its Session goes on working. CI builds the Runtime within the hosted runners' limits, and the size it would add to the wheel is measured.

### M7: Script parity

Goal: a Page's **Script Fingerprint** matches real Chrome's for its Persona and Machine, before breadth, as M1 did for the wire.

- A probe: a script that walks everything a page can reach from its global object. It records each interface, property and function: their order, attributes, `length`s and names, prototype chains, the source text of native functions, and the error each check throws. It also records the values that the Persona and the Machine decide, such as `navigator`, `screen`, the window's sizes, `matchMedia()`, `Intl`'s defaults and the time zone.
- **Baselines** of the probe, recorded by hand in real Windows Chrome for each Chrome Release in the **Supported Window**. They start with a top-level page, and get more **Recording Cases** as later milestones make them possible. They are recorded with the capture kit and kept as M1's are.
- A **Normal Form** for a Script Fingerprint, which hides what the machine decides as M5 does on the wire, and **Parity Checks** that gate CI.
- Bindings for every interface that Chrome exposes to a page, from the generator. A member whose behaviour is missing is a **Known Gap**, and M7 decides what it does when a page calls it.
- `navigator`, `screen`, `devicePixelRatio`, `matchMedia()`, `navigator.connection` and `navigator.userAgentData` from the Persona and the Machine, as [persona.md](persona.md#the-runtime) lists them. The Machine gains what the Runtime reads and HTTP does not, such as the time zone, the number of processors and the window's place ([ADR-0007](adr/0007-a-machine-of-its-own-per-session.md)).
- A Persona of an older Chrome Release turns off the V8 and Blink features that its release did not ship ([ADR-0008](adr/0008-v8-and-bindings-from-web-idl.md)).
- **Exit:** Parity Checks of the Script Fingerprint pass for every Chrome Release in the Supported Window, on every Host, with Known Gaps for missing behaviour only.

### M8: Documents

Goal: real pages load and run as they do in Chrome.

- HTML parsing as the specification has it, with its error recovery, `document.write()`, and `async`, `defer` and module scripts.
- The DOM and its events: mutation observers, event dispatch, and `requestAnimationFrame()` at the Machine's refresh rate.
- Navigation: `readyState`, `DOMContentLoaded` and `load`, `location` and history, redirects, `<meta http-equiv="refresh">` and form submission, each as a navigation through the Session.
- Frames, with documents of their own and Chrome's rules for access between origins. The Initiator learns which frame a request comes from.
- More Request Kinds, for what pages request: styles, fonts, frames, workers and `sendBeacon()`, and the `credentials` modes of `fetch()`.
- `localStorage`, `sessionStorage` and IndexedDB, saved with the Session as its cookies are.
- Dedicated workers.
- Tests from web-platform-tests. The Engine Release's tree records Chrome's own results for them, so mitto is held to Chrome's results, failures included.
- **Exit:** the chosen directories of web-platform-tests give Chrome's results, and the Parity Checks pass in frames and workers as well as in top-level pages.

### M9: Style, geometry and input

Goal: what a page measures through style and layout matches Chrome, and a caller can act as a user.

- CSS parsing, selectors, the cascade with Chrome's user agent stylesheet, computed style and the CSSOM.
- Layout for what scripts read: the boxes of elements, `getBoundingClientRect()`, the `offset`, `client` and `scroll` sizes, hit testing, `IntersectionObserver` and `ResizeObserver`. Nothing is painted.
- Text measured as Windows Chrome measures it, with the fonts a Windows machine has. M9 decides which fonts a Machine claims, and how mitto measures them without shipping Windows' fonts.
- The window and the screen from the Machine, the window's place included.
- Input from Python as trusted events: clicks, typing, mouse movement along a path and scrolling. They come with Chrome's sequence of pointer, mouse, keyboard, focus and input events, and with user activation, which decides `Sec-Fetch-User` on the navigations it starts.
- **Exit:** the chosen directories of web-platform-tests for CSS and geometry give Chrome's results, a caller's clicks and typing produce the events recorded from real Chrome, and the Parity Checks pass.

### M10: Graphics, and the Runtime ships

Goal: the rest of what pages commonly measure, and the first release with the Runtime.

- Canvas 2D on Skia, with text from M9's fonts.
- WebGL with the parameters, extensions and strings of a GPU that the Machine claims, drawn from a table like its other values.
- Web Audio's offline rendering.
- WebCrypto on BoringSSL.
- The Page API becomes public, with its reference in [api.md](api.md), and the release workflow puts the Page program in the wheel.
- **Left out:** WebSocket, WebRTC, WebGPU's behaviour, media playback, the behaviour of service workers, and an OS sandbox for the Page process ([ADR-0009](adr/0009-a-process-for-each-page.md)).
- **Exit:** the Parity Checks pass over the whole Script Fingerprint with every Known Gap written down, `pip install mitto` gives a working Runtime on every Host, and a minor version ships it with release notes.

## Cross-cutting rules

- C++ follows Chromium's style guide and allowed-features list ([ADR-0002](adr/0002-build-inside-chromium-tree.md)). Python follows ruff with strict type checking.
- Security: sanitizer builds (ASan, UBSan) run in CI. Every parser we write ourselves gets a fuzz target.
- Large artifacts (checkouts, build output, caches) live in the workspace, never in the repository.

## Risks

- **Field trials.** Chrome enables some network behavior through server-side experiments that a source build does not see. Mitigation: Baselines always come from real Chrome, never from our own build.
- **Randomized handshakes.** Chrome randomizes TLS extension order and GREASE values, so Parity Checks compare normalized forms rather than raw bytes.
- **Build cost.** Chromium builds are slow on ordinary developer machines. Building only the native core's targets, never the browser, keeps iteration usable.
- **Hosted runner limits.** GitHub-hosted runners have little disk space and a six-hour job limit, and their minutes are free only for public repositories. The trimmed export and build caching must keep a CI build inside those limits.
- **Windows toolchain.** Chromium takes Visual Studio and the Windows SDK from the machine, and a new milestone can ask for newer versions of them. A hosted image can lag behind: in October 2026, `windows-2025-vs2026` had Visual Studio 2026 but not the SDK that Chromium 157 asks for, so a job may have to install it first.
- **Breadth of the Script Fingerprint.** Chrome exposes hundreds of interfaces to a page, and each behaviour that is missing or differs is something a page can see. Mitigation: their shape is generated for all of them, missing behaviour is written down as Known Gaps, and Parity Checks and web-platform-tests show the rest.
- **Fonts and graphics.** What a page reads through text, canvas and WebGL comes from the fonts, the rasteriser and the GPU of a Windows machine, which a Linux or macOS Host lacks and which mitto cannot ship. Exact values may be out of reach, and M9 and M10 decide how close the Runtime comes.
- **Size and build time of the Runtime.** V8 and Chrome's ICU data make the wheel several times larger and CI builds longer. The trimmed export and the build caches must keep hosted runners within their limits, and M6 measures both.
- **Maintenance cadence.** Chrome ships a new stable milestone every two weeks, and falling behind makes the Persona stale. The routine, including its manual steps, has to be cheap and scripted.
