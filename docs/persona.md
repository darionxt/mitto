# Persona data

A **Persona** is the browser identity mitto presents to servers. Its data says what one **Chrome Release** on one operating system sends by default for each **Request Kind**. It is package data, so changing it needs no native rebuild. Terms in **bold** are defined in [CONTEXT.md](../CONTEXT.md).

`mitto.get()` presents Chrome 157 on Windows with the navigation Request Kind. Choosing a Persona or a Request Kind publicly comes with the Session API.

## The file

Each Persona is one file, `src/mitto/personas/NAME.json`, which [`src/mitto/_persona.py`](../src/mitto/_persona.py) reads:

| Key | Meaning |
| --- | --- |
| `release` | The Chrome Release |
| `operating_system` | The operating system the Persona claims, independent of the **Host** |
| `accept_language` | The value for `Accept-Language`, which is a Session setting |
| `kinds.KIND.priority` | The request's priority, such as `HIGHEST`, as in the C API's `MITTO_PRIORITY_HIGHEST` |
| `kinds.KIND.priority_incremental` | Whether the response can be used as it arrives |
| `kinds.KIND.referrer` | The value for `Referer`, or `null`. `<origin>` stands for the origin of the requested URL |
| `kinds.KIND.headers` | `name: value` lines in the order Chrome sends them, with the case HTTP/1.1 sends |

Subresource kinds describe one same-origin **Initiator**, the page at the root of the origin. Other Initiators come with the Session API.

## What comes from where

The file does not hold everything on the wire, because Chrome does not decide everything in one place either. mitto hands each part to `//net` the way Chrome's network service does, so that it lands where Chrome's does.

| On the wire | Comes from |
| --- | --- |
| `Host`, `Connection`, and the pseudo-headers of HTTP/2 and HTTP/3 | `//net` |
| The lines in `headers` | The C API's `headers`. HTTP/2 and HTTP/3 send the names in lower case |
| `Referer` | The C API's `referrer`. `//net` drops a `Referer` among the headers, and sends this one after them |
| `Accept-Encoding` | `//net`, after `Referer`. The native core turns on Brotli and Zstandard as Chrome does, and `//net` advertises them only over HTTPS and to localhost |
| `Accept-Language` | `//net`, after `Accept-Encoding`, from the Session setting |
| The `priority` header, last in HTTP/2 and HTTP/3 and absent in HTTP/1.1 | `//net`, from `priority` and `priority_incremental` |
| The HTTP/2 stream weight and the HTTP/3 `PRIORITY_UPDATE` | `//net`, from the same two values |

So a line for `Referer`, `Accept-Encoding`, `Accept-Language` or `priority` does not belong in `headers`. Giving `Referer` there is an error, and giving the others would put them in the wrong place, or send `priority` over HTTP/1.1 as well.

## Write one

Write the file from the Baselines of its Chrome Release in [`tests/baselines`](../tests/baselines), by taking away what the table above leaves to `//net`. Over the three protocols, a Request Kind differs only in what `//net` adds and in the case of its names, so one list of lines serves all three.

[`tests/test_persona.py`](../tests/test_persona.py) compares the file with every Baseline of its Chrome Release, including HTTP/3, without a native library. It writes out what `//net` adds. The [Parity Checks](parity.md) then show what `//net` really does, as far as the native core reaches each protocol.

## What no Baseline covers

Baselines only record HTTPS. Over plain HTTP to a host that is not local, Chrome leaves out `sec-ch-ua*` and `Sec-Fetch-*`, because those URLs are not potentially trustworthy, and the data does not change for that. `//net` does make the change for `Accept-Encoding`.
