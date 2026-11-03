---
status: accepted
---

# The Chrome network stack, not the Python layer, owns HTTP semantics

The Chrome network stack decides redirects, cookies, caching, authentication challenges and connection reuse. The Persona and Request Kind decide default headers. The Python layer only describes requests and reads responses. Its API looks familiar to people who know `requests`, but it is deliberately not compatible with it and does not depend on it. A drop-in replacement would have to sit under the `requests` session, which applies its own redirect, cookie and header rules on top. That would break Chrome parity in ways Parity Checks would keep catching.

## Considered Options

- **Subclass the `requests` session and plug the native core in as a transport adapter.** Users could swap imports and nothing else. It was rejected because two layers would make the same HTTP decisions with different rules.

## Consequences

- Code moving from `requests` needs small changes. Documentation should show the common mappings.
