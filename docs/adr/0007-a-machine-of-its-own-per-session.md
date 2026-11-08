---
status: accepted
---

# Each Session claims a machine of its own, drawn by default

A Persona is one Chrome Release on Windows, and today it also claims one machine. So every Session that a server asks for client hints gives the same memory, display, window, network, user settings and Windows version, and all of them land on one point, which a server can count. So each Session gets a **Machine** of its own, apart from its Persona: drawn by default from a weighted table that ships with mitto, repeatable from a seed, with any value fixed by the caller, and saved with the cookies so that a Session that comes back is the same machine. It is drawn the same way on every Host, and never read from the machine mitto runs on. What decides each value in Chrome, and the statistics behind the table, are in [persona.md](../persona.md#a-machine-of-its-own).

## The decision

- **What a Machine holds.** What Chrome reads, and not what it sends: the memory as Chrome rounds it, the primary display's size in pixels and its scale, the viewport, the round trip and throughput as estimated before noise, the colour scheme, reduced motion, reduced transparency and the Windows version as `sec-ch-ua-platform-version` writes it. A Session works out each hint from these, as Chrome does. The architecture, bitness, WoW64, model and form factors stay with the Persona, which states x64 Windows.
- **How it is drawn.** Groups that depend on each other are drawn together, from one table of weighted rows: the display with its scale and its viewport, and the round trip with the throughput. The other values are drawn on their own. A drawn Machine keeps its seed. The same seed and table give the same Machine. A later mitto that knows more fields draws the missing ones from the seed, to fit those it has, so a saved Machine never changes.
- **Where the weights come from.** StatCounter for Windows 10 against 11 and for the display in CSS pixels, which carries the scale, and the Firefox Public Data Report for the memory and the display in pixels. Where no public source has weights, the table says so and takes the owner's:
  - Windows 11 is `19.0.0`, which 24H2 says and 25H2 and 26H2 most likely say, and Windows 10 is `10.0.0`, which its last versions say. Microsoft moves most users to those versions. The others wait for a source.
  - Reduced motion and reduced transparency stay at `no-preference`, which Windows has as it comes, so they need no draw.
  - The colour scheme is `dark` at a share the owner sets. Chrome Platform Status shows about 40% of page loads on every platform, but its data has no licence and no Windows split.
  - The round trip and throughput are Chrome's own starting values for Ethernet and Wi-Fi, which a new Chrome sends, until a desktop table is worked out from the Chrome UX Report.
- **What a caller does.** Nothing, for a drawn Machine. A seed, to draw the same one again. Any value fixed, with the rest drawn to fit it, or a whole Machine given. A sketch, for the work to settle: `Session(machine=Machine.draw(seed=7, memory=16))`, and `session.machine` to read it.
- **Where it is saved.** `cookie_file` and `Cookies.save()` write the Machine beside the cookies, in a new version of the file that the Python layer splits, so the native core still gets the cookies alone. A file of version 1, from mitto 0.1, loads with the Machine that every Session claimed then.
- **Noise.** The salts of `rtt` and `downlink` belong to a run of Chrome and not to the machine, so they are drawn each time a Session opens, from its seed if it has one, and never saved. The native core works out `rtt` and `downlink` on each hop, for the host of that hop, with Chrome's two hashes.
- **Parity Checks.** `sec-ch-ua-platform-version` joins the hints that the normal form hides as `<machine>`, and the Baselines are written again with that value changed. Tests of the rounding against Chromium's own cases, of each drawn value against what Chrome can send, and of what a Session sends on each hop check the values. A check where the recordings are kept compares the form of real Chrome's hidden values with a Session's.

The term is new, and [CONTEXT.md](../../CONTEXT.md) has it:

> **Machine**:
> The computer a Session claims to run on: its memory, display, window, network, its user's settings and its Windows version. It is independent of the Host and of the Persona.
> _Avoid_: device, hardware profile, environment, fingerprint

The Persona's entry says "with its locale and the client hints of its Chrome", since the values that follow the machine move here, and the Session's entry names its Machine.

## Considered Options

- **Fixed by default, with a draw as an option.** Nothing changes for callers, but every Session that does not ask stays on one point, which is the problem this solves.
- **Always given by the caller.** Nothing is hidden, but most callers would copy one example and land on one point again, and each would have to learn which values go together.
- **One Machine per Persona, or more Personas.** Still one point for each Persona, and the Persona's file would no longer be what one Chrome Release sends.
- **A new Machine for each request.** A machine does not change between requests, and a server that sees the same cookies with another display can tell.
- **Read from the Host.** The Persona claims Windows on every Host, so a Linux or macOS Host has nothing to read, and on a Windows Host it would hand a server the real machine. The owner ruled it out on 2026-11-08.
- **Keep what Chrome sends, such as `dpr` and the viewport, and not the display.** Simpler for HTTP, but the Runtime's `screen`, `devicePixelRatio` and window sizes could then disagree with the hints, and `rtt` could not get noise for each host.

## Consequences

- What a Session sends to an origin that asks for these hints changes, and so does the cookie file, which mitto 0.1 cannot read. This ships in a minor version, with release notes that say so.
- Parity Checks no longer compare the value of `sec-ch-ua-platform-version`. The tests above take that over, and what a draw can give is only as good as the table.
- The table is package data under CC BY-SA 3.0, the licence of StatCounter's and Mozilla's data, with their credit, so the wheel declares `Apache-2.0 AND CC-BY-SA-3.0`. The code keeps Apache-2.0. If that is not acceptable, the table can only come from sources with no share-alike, and today none has the display or the memory.
- A step of the release routine fetches the sources again and shows the table's diff for review.
- The native core ports `std::hash` of libc++ and Blink's hash of a string, with the rounding, for `rtt` and `downlink`.
- The Runtime reads the same Machine for `navigator.deviceMemory`, `screen`, `devicePixelRatio`, `matchMedia()` and `navigator.connection`, and adds fields to it, such as the window's place, as it needs them.
