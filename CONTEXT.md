# mitto

mitto is a Python library that talks to web servers the way Chrome does, so that what a server can observe about the traffic matches a real Chrome. It starts as an HTTP client; a headless JavaScript runtime is planned as a second phase.

## Language

### Identity

**Persona**:
The browser identity mitto presents to servers: one Chrome Release on one operating system, with its locale and client-hint values.
_Avoid_: profile, fingerprint, identity, user agent

**Chrome Release**:
A Chrome stable milestone (for example 156) whose observable behavior a Persona reproduces.
_Avoid_: version, build

**Supported Window**:
The Chrome Releases mitto can present: the current stable release and the two before it.
_Avoid_: version range

**Engine Release**:
The one Chrome Release whose actual network code a given mitto release ships; always the newest Chrome Release in the Supported Window.
_Avoid_: engine version, Chromium version

**Host**:
The operating system and CPU architecture mitto itself runs on. It is independent of the operating system the Persona claims.
_Avoid_: platform

### Requests

**Session**:
A long-lived client that holds one Chrome-like browsing state: its Persona, cookies, cache and open connections.
_Avoid_: client, browser, context

**Request Kind**:
Which kind of Chrome request is being reproduced, such as a top-level navigation or a page's fetch, script or image request. It decides Chrome's default headers and priority for that request.
_Avoid_: request type, mode

**Initiator**:
The page a non-navigation request is made from. It decides whether the request counts as same-origin, same-site or cross-site.
_Avoid_: referrer, document URL, origin

### Fidelity

**Wire Fingerprint**:
Everything a server can observe about a request below the application payload: the TLS handshake, HTTP/2 and QUIC parameters, and the header names, order and values.
_Avoid_: TLS fingerprint, JA3, JA4

**Baseline**:
A recorded Wire Fingerprint of real Chrome for one Persona and Request Kind, used as ground truth.
_Avoid_: golden file, reference capture

**Parity Check**:
An automated comparison of mitto's Wire Fingerprint against a Baseline.
_Avoid_: fingerprint test, diff

### Later phase

**Runtime**:
The planned headless JavaScript environment, without rendering, that runs a page's scripts against a Chrome-like DOM and Web APIs.
_Avoid_: browser, headless browser, engine
