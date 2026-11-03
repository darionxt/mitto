---
status: accepted
---

# Use Chromium's own network stack, not an imitation of it

The Wire Fingerprint is mitto's core value, so the networking is Chromium's own `//net`, with its pinned BoringSSL and QUIC implementation, built from the Engine Release's source. It is driven directly at the `URLRequest` level and configured the way Chrome's network service configures it. We do not reproduce Chrome's TLS and HTTP/2 parameters on top of another HTTP or TLS library: an imitation must chase every detail by hand, such as extension order randomization, ALPS, post-quantum key shares, HTTP/2 settings and priorities, and QUIC transport parameters, while the real stack gets them right by construction.

## Considered Options

- **Patch another HTTP client and TLS library to send Chrome's parameters.** Small and quick to build, but parity is only as good as the last manual update, and behavior below the parameters (connection pooling, retries, Alt-Svc, coalescing) still differs.
- **Embed Cronet.** Packages `//net` officially, but its embedder defaults differ from Chrome's network service and its API hides settings we need to control.

## Consequences

- The build is heavy, and every Chrome stable release (about every four weeks) requires an engine update.
- Chrome ships some behavior through server-controlled field trials that a source build does not see. The configuration must state those features explicitly, and Parity Checks against Baselines from real Chrome are the only proof of parity.
