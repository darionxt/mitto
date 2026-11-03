---
status: accepted
---

# Build the native core inside a pinned Chromium tree, in Chromium's C++ dialect

The native core is a set of GN targets compiled inside a Chromium checkout pinned to the Engine Release tag, with Chromium's own toolchain, libc++ and build flags. `//net` has no standalone build: it depends on `//base` and on Chromium's toolchain. Mixing separately built libraries with another C++ standard library breaks the ABI, and build flags that differ from Chrome's can change observable behavior. Because the code lives in Chromium's build, all C++ follows the Chromium C++ style guide and its allowed-features list (`styleguide/c++/c++-features.md`). In practice that means no exceptions, `base::` types instead of banned standard ones, and Chromium naming. Generic C++ guidelines that conflict with it do not apply.

## Consequences

- Continuous integration (CI) cannot check out all of Chromium on every run. For each Engine Release we produce a trimmed source export that holds only what the native core needs, and CI builds from that export.
- Code ported from Chromium keeps its upstream structure and names, so it can be compared with and re-synced against upstream.
