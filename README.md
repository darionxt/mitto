# mitto

mitto is a Python HTTP client whose traffic matches real Chrome. It does not imitate Chrome's network code: it runs it. Chromium's own network stack, `//net` with BoringSSL and QUICHE, is built from Chromium's source into a native library, so a server sees Chrome's TLS handshake, Chrome's HTTP/2 and QUIC settings, and Chrome's headers in Chrome's order, over HTTP/1.1, HTTP/2 and HTTP/3.

Terms in **bold** are defined in [CONTEXT.md](https://github.com/darionxt/mitto/blob/main/CONTEXT.md).

## Install

```bash
pip install mitto
```

mitto installs on two **Hosts**, macOS on arm64 and Linux on x86_64, with Python 3.12 or later. Each Host has a wheel of its own with the native library inside, so mitto needs no Chrome, no compiler and no other package. There is no source distribution, because building the native library takes Chromium's toolchain, so pip finds nothing to install on any other Host. [building.md](https://github.com/darionxt/mitto/blob/main/docs/building.md) says how to build it yourself.

## What a server sees

A **Session** presents a **Persona**: one **Chrome Release** on Windows, with its `User-Agent`, client hints, `Accept-Language` and the headers of each kind of request. A Session on macOS or Linux presents Windows Chrome all the same. Cookies, redirects and the HTTP cache are Chrome's code too, so which requests go out, and with which cookies, follows Chrome's rules.

mitto presents the current stable Chrome Release and the two before it, its **Supported Window**, on the network code of the newest, its **Engine Release**. New versions follow Chrome's stable milestones, and a Session presents the Engine Release unless [`persona=`](https://github.com/darionxt/mitto/blob/main/docs/api.md#persona) names another.

This version presents Chrome 155, 156 and 157, with Chrome 157 as its Engine Release.

[persona.md](https://github.com/darionxt/mitto/blob/main/docs/persona.md) says what each Persona sends, and [parity.md](https://github.com/darionxt/mitto/blob/main/docs/parity.md) how that is checked against **Baselines** recorded from real Chrome.

## Use it

A Session holds one browsing state, as a Chrome profile does: cookies, cache and open connections. Close it, or use a `with` statement.

```python
import mitto

with mitto.Session() as session:
    response = session.get("https://example.com/")
    response.raise_for_status()
    print(response.status, response.protocol, response.headers["content-type"])
    print(response.text)
```

A `GET` goes as a navigation and another method as a page's fetch, unless `kind=` says otherwise, and `initiator=` names the page a request comes from. The [API reference](https://github.com/darionxt/mitto/blob/main/docs/api.md) has every argument.

`mitto.AsyncSession` has the same arguments, the same errors and the same browsing state, and many requests at once need no thread each.

```python
import asyncio

import mitto


async def main():
    async with mitto.AsyncSession() as session:
        return await asyncio.gather(
            *(session.get("https://example.com/", params={"page": page}) for page in ("1", "2", "3"))
        )


for response in asyncio.run(main()):
    print(response.url, response.status)
```

mitto looks familiar to people who know `requests`, but it is deliberately not compatible with it. [from-requests.md](https://github.com/darionxt/mitto/blob/main/docs/from-requests.md) has what changes.

## What it does not do yet

mitto is an HTTP client: it runs no JavaScript and renders nothing. A headless JavaScript **Runtime** is planned as a later phase.

The HTTP client leaves out WebSocket, CORS preflights, HTTPS upgrades of navigations, an `auth=` argument for origins (an `Authorization` header works), `verify=False`, client certificates, Certificate Transparency enforcement, Reporting and NEL, shared dictionaries and preconnects. A Session also sends none of the lines Chrome adds for Google's domains, which [parity.md](https://github.com/darionxt/mitto/blob/main/docs/parity.md#googles-domains) lists. The [roadmap](https://github.com/darionxt/mitto/blob/main/docs/roadmap.md) has the plan.

## Documentation

- [API reference](https://github.com/darionxt/mitto/blob/main/docs/api.md): every public name, with examples.
- [From `requests` to mitto](https://github.com/darionxt/mitto/blob/main/docs/from-requests.md): what changes for code that uses `requests`.
- [Persona data](https://github.com/darionxt/mitto/blob/main/docs/persona.md): what a request sends, and why.
- [Parity Checks](https://github.com/darionxt/mitto/blob/main/docs/parity.md): how mitto's traffic is compared with Chrome's, and what stays different.
- [The capture kit](https://github.com/darionxt/mitto/blob/main/docs/capture.md) and [Recording Baselines](https://github.com/darionxt/mitto/blob/main/docs/baselines.md): how real Chrome is recorded.
- [Building](https://github.com/darionxt/mitto/blob/main/docs/building.md): the native library, from a Chromium checkout or from the source export.
- [Roadmap](https://github.com/darionxt/mitto/blob/main/docs/roadmap.md) and [decisions](https://github.com/darionxt/mitto/tree/main/docs/adr): the plan, and why it is this way.

## Licence

mitto is licensed under the [Apache License 2.0](https://github.com/darionxt/mitto/blob/main/LICENSE).
