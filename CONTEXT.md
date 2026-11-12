# mitto

mitto is a Python library that talks to web servers the way Chrome does, so that what a server can observe about the traffic matches a real Chrome. It started as an HTTP client. Its second phase is the Runtime, which runs a page's scripts.

## Language

### Identity

**Persona**:
The browser identity mitto presents to servers: one Chrome Release on one operating system, with its locale and the client hints of its Chrome.
_Avoid_: profile, fingerprint, identity, user agent

**Chrome Release**:
A Chrome stable milestone (for example 157) whose observable behavior a Persona reproduces.
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

**Machine**:
The computer a Session claims to run on: its memory, processors, display, window, touch screen, network, time zone, its user's settings and its Windows version. It is independent of the Host and of the Persona.
_Avoid_: device, hardware profile, environment, fingerprint

### Requests

**Session**:
A long-lived client that holds one Chrome-like browsing state: its Persona, its Machine, cookies, cache, open connections and open Pages.
_Avoid_: client, browser, context

**Request Kind**:
Which kind of Chrome request is being reproduced, such as a top-level navigation or a page's fetch, script or image request. It decides Chrome's default headers and priority for that request.
_Avoid_: request type, mode

**Initiator**:
The page a request is made from, such as the page that holds a clicked link. It decides whether the request counts as same-origin, same-site or cross-site. A navigation the user starts elsewhere, such as from the address bar, has none.
_Avoid_: referrer, document URL, origin

### Pages

**Runtime**:
The headless JavaScript environment, without rendering, that runs a Page's scripts against a Chrome-like DOM and Web APIs.
_Avoid_: browser, headless browser, engine

**Page**:
What a tab holds in Chrome, in a Session: the document loaded in it, its frames, its scripts and their state, run by the Runtime in a process of its own. Its requests are its Session's.
_Avoid_: tab, window, browser context, document

### Fidelity

**Wire Fingerprint**:
Everything a server can observe about a request below the application payload: the TLS handshake, HTTP/2 and QUIC parameters, and the header names, order and values.
_Avoid_: TLS fingerprint, JA3, JA4

**Script Fingerprint**:
Everything a page's scripts can observe about where they run: which interfaces, properties and functions exist, in what order and with which attributes, what they return and how they fail, and the values that come from the Persona and the Machine.
_Avoid_: JS fingerprint, browser fingerprint, environment

**Recording Case**:
One situation in which real Chrome is recorded: a Request Kind, how its Initiator relates to where it goes, and what it carries, such as a cross-site fetch with a text body. A hop of a redirect chain is a Recording Case of its own, and so is a same-origin request of each main Request Kind. For a Script Fingerprint, it is where the scripts run, such as a top-level page, a frame of another site or a worker.
_Avoid_: scenario, test case, fixture

**Baseline**:
A recorded Wire Fingerprint or Script Fingerprint of real Chrome for one Persona and Recording Case, used as ground truth.
_Avoid_: golden file, reference capture

**Normal Form**:
A Wire Fingerprint or Script Fingerprint written so that two recordings of the same client compare equal: what the client randomizes drops out or becomes a placeholder, and only what it decides stays.
_Avoid_: canonical form, normalized capture

**Parity Check**:
An automated comparison of mitto's Wire Fingerprint or Script Fingerprint against a Baseline.
_Avoid_: fingerprint test, diff

**Known Gap**:
A difference from a Baseline that is written down in the repository, so that a Parity Check accepts it until the difference is fixed.
_Avoid_: exception, allowlist, expected failure
