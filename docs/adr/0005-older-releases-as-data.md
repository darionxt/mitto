---
status: accepted
---

# Older Chrome Releases are emulated as data on the newest engine

Each mitto release ships exactly one network engine, built from the Engine Release. The other Chrome Releases in the Supported Window are reproduced by Persona data on top of that engine: TLS handshake parameters, HTTP/2 and QUIC settings, and per-release default headers and client hints. Shipping one engine per Chrome Release would multiply the package size and the build matrix. Within a two-release distance, the differences that servers can observe are parameters, not code.

## Consequences

- A behavior change between Chrome Releases that cannot be expressed as data means that Chrome Release stays emulated imperfectly. Parity Checks must cover every Chrome Release in the Supported Window, not only the Engine Release.
- The Supported Window stays small on purpose. Widening it is a new decision, not a configuration change.
