---
status: accepted
---

# The Runtime runs the Engine Release's V8, with bindings generated from Chromium's Web IDL

The **Runtime** runs a page's scripts, and what they can observe, its **Script Fingerprint**, has to match the Chrome its Persona claims. Most of that comes from the JavaScript engine and from the shape of the Web APIs, so both come from Chromium: V8 from the Engine Release, built in the same tree with the settings Chrome builds it with, and bindings generated from the Engine Release's Web IDL files. What sits behind the bindings, the DOM, HTML parsing, CSS and layout, is written for mitto. It follows the specifications and Blink's behaviour, and takes Blink's data where that is all it needs.

## The decision

- **V8.** The Engine Release's V8, with Chrome's build settings on 64-bit desktops: its compiler tiers, pointer compression and V8's sandbox. Builtins, their error messages, `Function.prototype.toString()`, stack traces, WebAssembly and garbage collection then behave as in that Chrome. `Intl` does too, because V8 reads the whole ICU data file that Chrome ships, not the few items that the native core reads today.
- **Bindings.** A generator reads the Engine Release's Web IDL files, as Blink's build does. For each interface that Chrome exposes to a page, with the features that Chrome turns on for stable on Windows, it writes V8 bindings: members in the order Chrome installs them, with their attributes, `length`s and names, and the messages of Chrome's type and brand checks. So a page sees every interface Chrome has, whether or not mitto implements its behaviour yet. A member whose behaviour mitto lacks is a **Known Gap**, written down as the wire's are.
- **Behind the bindings.** The DOM, HTML parsing, the event loop, CSS, style and layout are written in the native core's C++, in Chromium's style (ADR-0002). They follow the specifications, and Blink where Blink differs. Some of Blink is data, and that data is used directly: its user agent stylesheet, its list of CSS properties and their order, and its names of HTML elements and attributes. Chromium code that builds without Blink and `//content` is used as it is, such as `//url`, BoringSSL behind WebCrypto, Skia and HarfBuzz.
- **Older Chrome Releases.** For the network, the older Chrome Releases are data on one engine (ADR-0005), and the Runtime follows the same rule: one V8 and one set of bindings ship, the Engine Release's. A Persona of an older Chrome Release turns off what its release did not have yet: through V8's flags for features that V8 shipped later, and through Blink's list of runtime-enabled features for those not yet on for stable in that release. Whatever cannot be turned off is a Known Gap of that Persona.

The term is new, and [CONTEXT.md](../../CONTEXT.md) has it:

> **Script Fingerprint**:
> Everything a page's scripts can observe about where they run: which interfaces, properties and functions exist, in what order and with which attributes, what they return and how they fail, and the values that come from the Persona and the Machine.
> _Avoid_: JS fingerprint, browser fingerprint, environment

## Considered Options

- **Blink itself, without the rest of Chrome.** Everything a page sees would be right by construction, as `//net` makes the wire right (ADR-0001). But Blink reaches the rest of Chrome through `//content` and the browser process's Mojo services, and mitto would have to stand in for all of them. Blink's build is also many times the size of the native core's. A hosted runner could not be expected to finish it from cold within its six-hour limit, and every Engine Release, every two weeks, starts it again.
- **Another JavaScript engine.** Smaller, but every builtin, error message, stack trace and timing differs from V8's, and a page can tell in a few lines of script.
- **V8 without its compilers (`--jitless`).** Less code writes executable memory, but a page can measure the slowdown. V8 then also leaves `WebAssembly` off the global object, unless it is built with an experimental interpreter.
- **Bindings written by hand.** The order of members, their attributes and Chrome's messages would have to be copied by hand for every interface, and copied again for each Engine Release.

## Consequences

- Each Web API that is missing or behaves differently is something a page can see. Parity Checks of the Script Fingerprint against Baselines from real Chrome show where the Runtime stands, as they do for the wire.
- Each Engine Release brings a new V8 and new Web IDL files. The release routine generates the bindings again. An interface new in that release appears with its behaviour missing until someone writes it.
- What a page measures through text, canvas, WebGL and audio depends on the fonts and graphics of a Windows machine, which a Host may not have and mitto cannot ship. The milestones that build them decide how the Machine claims them.
