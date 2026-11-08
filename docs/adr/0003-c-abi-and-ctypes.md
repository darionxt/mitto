---
status: accepted
---

# Cross the Python boundary through a plain C ABI loaded with ctypes

The native core exports a small C API: opaque handles, byte buffers and events that wait in a native queue. The Python package loads that API with `ctypes`, and each Host gets one `py3-none` wheel. A CPython extension would have to compile Python headers with Chromium's toolchain and expose C++ built against Chromium's private libc++. With a C boundary, every C++ symbol stays hidden inside one shared library, and the wheel does not depend on the Python minor version. The cost of a `ctypes` call does not matter at request granularity.

## Considered Options

- **A CPython extension built with nanobind or pybind11, using the stable ABI.** Possible since we require Python 3.12 or later, and the call overhead is lower. It was rejected because it couples the Python build to the Chromium build and leaks C++ types across the boundary.
- **Completion callbacks through `ctypes`.** This decision first named these, and M2 replaced them before anything was built on them. A `ctypes` callback runs Python on the network thread, which has to take the GIL first, so the traffic of every Session would wait for whatever Python code holds it. A thread that asks for the GIL during interpreter shutdown is ended or hung, depending on the Python version, and the network thread cannot be either.
- **A file descriptor per request.** It is the simplest thing to wait on, but a pipe takes two descriptors, so a few hundred requests would reach the default limit on open files. Most of them would only be waiting for one of a host's six connections.

## Consequences

- The C header is the contract between the two halves and must stay versioned and minimal.
- A Session's engine settings and a request's options cross the boundary as versioned JSON buffers, so a new one does not change the ABI. The native core refuses what it does not know, so that a mistake cannot quietly change what goes on the wire.
- The network thread never runs Python and never waits for the GIL. A request is a handle with events: they wait in a native queue, and body bytes are read only when the caller asks, so a slow reader slows the server through flow control and nothing buffers a whole response.
- A sync caller blocks in a native call with the GIL released. On macOS and Linux an asyncio caller watches one file descriptor per Session, which is readable while any of its requests has an event waiting.
- On Windows an asyncio caller could not watch such a descriptor: `ProactorEventLoop`, the default there, has no `add_reader()`, and `SelectorEventLoop` watches sockets only. So there a thread per Session blocks in a native call with the GIL released, until the Session's list of requests with events fills, and hands each wake-up to the loop with `call_soon_threadsafe()`, which works with either loop. The call returns once each time the list fills, and waits again until the loop has emptied it, so the thread sleeps for as long as the loop is busy, however many events wait. Ending the waits is a call of its own, so that the thread can be joined before the Session is destroyed: a call that started after `mitto_session_destroy()` would use a Session that has gone. The network thread still never runs Python.
