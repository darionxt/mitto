---
status: accepted
---

# Each Page runs in a process of its own, and its requests go through its Session

A **Page** runs scripts that its site wrote, and some sites are hostile. Chrome runs them in a renderer process, apart from the browser and its network service, and the Runtime does the same. For each Page, the native core starts a process from a program in the wheel, and the Page's scripts, DOM and V8 heap live there. The Session stays in the Python process with its network, cookies, cache and connections, and the Page asks it for everything that touches them.

## The decision

- **One process for each Page.** `Session.open_page()` makes the native core start the Page program, which is built from the same tree as the library and shipped beside it in the wheel. Every frame of the Page runs in that process. The bindings limit what a frame can reach in a frame of another origin, as Chrome limits it. Closing the Page or its Session ends the process.
- **The Session owns the state.** The Page process has no network, cookie store, HTTP cache or files of its own. Its navigations, subresources, `fetch()` and `XMLHttpRequest` go to its Session, with the Request Kind and the Initiator that the Page works out. So they share the Session's connections, cookies and cache, and send what a request of that Kind sends. `document.cookie` reads and writes the Session's cookies. A Page and the caller's own requests on the same Session are one browsing state, as a tab and the address bar of one Chrome profile are.
- **The channel.** The two processes talk over one channel, in versioned messages. Each side refuses a message it does not know, as the native core refuses settings it does not know (ADR-0003). Response bodies stream over it with flow control, so a Page that reads slowly slows the server down, as a request's reader does. M6 chooses between Chromium's Mojo and a channel of the native core's own.
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

## Consequences

- The wheel carries the Page program, and with it V8 and Chrome's ICU data. There is still one wheel per Host, for HTTP and the Runtime alike: the owner chose that over a second distribution for the Runtime. So a caller who only uses HTTP downloads the Runtime too.
- Until the release that ships the Runtime, the release workflow leaves the Page program out of the wheel, and the Page API stays private. Releases before then do not grow.
- Each Page costs a process and a V8 heap, some tens of megabytes.
- The Page process needs no files or network access of its own, so Chromium's sandbox fits it, and the Runtime does not ship without it (roadmap, M10). Until the sandbox is in, a Page process runs with the caller's rights, but only builds from `main` have one, because releases leave the Page program out.
- The asyncio wait of each Host (ADR-0003) covers Pages as well as requests.
- Request Kinds grow to cover what pages request, such as styles, fonts, frames and workers. The Initiator, which today is always a top-level page, learns frames.
