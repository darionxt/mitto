---
status: accepted
---

# Each Page runs in a process of its own, and its requests go through its Session

A **Page** runs scripts that its site wrote, and some sites are hostile. Chrome runs them in a renderer process, apart from the browser and its network service, and the Runtime does the same. For each Page, the native core starts a process from a program in the wheel, and the Page's scripts, DOM and V8 heap live there. The Session stays in the Python process with its network, cookies, cache and connections, and the Page asks it for everything that touches them.

## The decision

- **One process for each Page.** `Session.open_page()` makes the native core start the Page program, which is built from the same tree as the library and shipped beside it in the wheel. Every frame of the Page runs in that process. The bindings limit what a frame can reach in a frame of another origin, as Chrome limits it. Closing the Page or its Session ends the process.
- **The Session owns the state.** The Page process has no network, cookie store, HTTP cache or files of its own. Its navigations, subresources, `fetch()` and `XMLHttpRequest` go to its Session, with the Request Kind and the Initiator that the Page works out. So they share the Session's connections, cookies and cache, and send what a request of that Kind sends. `document.cookie` reads and writes the Session's cookies. A Page and the caller's own requests on the same Session are one browsing state, as a tab and the address bar of one Chrome profile are.
- **The channel.** The two processes talk over one channel, in versioned messages. Each side refuses a message it does not know, as the native core refuses settings it does not know (ADR-0003). Response bodies stream over it with flow control, so a Page that reads slowly slows the server down, as a request's reader does. The channel is the native core's own, on `//base`, not Chromium's Mojo. The Page process inherits one end of a socket pair on macOS and Linux, and of a named pipe on Windows, and the library's network thread reads the other end. Each message is a frame that carries its length and its type. Each side first says which version it speaks and refuses any other, because the two programs ship together. The Session sends a body only as far as the Page has given it credit, and the Page gives credit back as it reads.
- **Python.** Python talks to a Page through the C API, as it does to a request: calls that return at once, and events in a native queue. No Python runs on any native thread. A sync caller blocks with the GIL released, and an asyncio caller waits as it does for requests on each Host. A sketch, for M6 to settle: `with session.open_page(url) as page: page.evaluate("document.title")`, and `await session.open_page(url)` on the asyncio Session.
- **Time.** A Page runs on the real clock, with Chrome's coarsening of `performance.now()` and of timers. There is no virtual time, because a page measures how long its work takes.
- **Sandbox.** Before the Runtime ships, the Page process runs in Chromium's sandbox on every Host, with no access to files or the network of its own, as Chrome's renderers run. Without a sandbox, the process stops a page's crash but not its exploit.
- **Failure.** If the Page process crashes, runs out of memory or is killed, the Page raises an error on the calls in progress and on its next call, and the Session goes on working. Nothing in a Page can end the Python process.

The term is new, and [CONTEXT.md](../../CONTEXT.md) has it:

> **Page**:
> What a tab holds in Chrome, in a Session: the document loaded in it, its frames, its scripts and their state, run by the Runtime in a process of its own. Its requests are its Session's.
> _Avoid_: tab, window, browser context, document

## Considered Options

- **The Python process.** No second process and no channel, and lighter. But V8 ends its process when a heap cannot grow and when one of its checks fails, so a page could end the caller's program. A page that exploited a V8 bug would also run with every right that program has.
- **One process for each Session.** Fewer processes, but a crash in one Page would end every Page of its Session, and Pages of different sites would share a heap.
- **One process for each site, as Chrome's site isolation splits frames.** Closest to Chrome, but it puts the frames of one Page in several processes and needs Chrome's frame tree across processes, for a difference that a page can barely see. It can come later on the same channel.
- **Chromium's Mojo for the channel**, as Chrome's renderers use it, with bindings generated from `.mojom` files, data pipes for bodies and a check of each message. Mojo is built for many interfaces between many processes, for handles that pass between them and for programs that update apart. The library and the Page program are two programs from one build, with one channel between them. Mojo would also keep state for the whole process in the caller's program, which would become its broker. On macOS it would register a service with launchd from there to hand the Page its Mach port, and if a second Chromium-based library in the same program did the same, a failed check would end the program. Its sandbox would need that lookup, shared memory and handles that the parent duplicates into the Page, where the own channel needs one descriptor or handle. Its code generator, its core and ipcz would join every build and the source export. In bytes it costs little, about half a megabyte in each program, so that is not what decides it.

## Consequences

- The wheel carries the Page program, and with it V8 and Chrome's ICU data. There is still one wheel per Host, for HTTP and the Runtime alike: the owner chose that over a second distribution for the Runtime. So a caller who only uses HTTP downloads the Runtime too.
- Until the release that ships the Runtime, the release workflow leaves the Page program out of the wheel, and the Page API stays private. Releases before then do not grow.
- Each Page costs a process and a V8 heap, some tens of megabytes.
- The Page process needs no files or network access of its own, so Chromium's sandbox fits it, and the Runtime does not ship without it (roadmap, M10). Until the sandbox is in, a Page process runs with the caller's rights, but only builds from `main` have one, because releases leave the Page program out.
- The asyncio wait of each Host (ADR-0003) covers Pages as well as requests: the network thread reads the channel and lists a Page with an event beside the requests that have one.
- The Page process holds only its end of the channel and what it gets when it starts, so its sandbox only has to let it read and write that one descriptor or handle.
- The Session reads each frame that a Page sends as hostile input, and the frame reader gets a fuzz target, as each parser that mitto writes does. The messages and their checks are written by hand. If later milestones bring many more of them, a generator of mitto's own can come then.
- The Page process ends when the Python process dies without closing it, because it reads the end of its channel. For when that end does not come, such as when a fork of the Python process still holds it, the Page process also watches its parent on macOS. On Linux the kernel signals it when its parent dies, and on Windows it runs in a job object that closes with the Python process.
- Request Kinds grow to cover what pages request, such as styles, fonts, frames and workers. The Initiator, which today is always a top-level page, learns frames.
