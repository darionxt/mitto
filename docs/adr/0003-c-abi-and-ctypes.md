---
status: accepted
---

# Cross the Python boundary through a plain C ABI loaded with ctypes

The native core exports a small C API: opaque handles, byte buffers and completion callbacks. The Python package loads that API with `ctypes`, and each Host gets one `py3-none` wheel. A CPython extension would have to compile Python headers with Chromium's toolchain and expose C++ built against Chromium's private libc++. With a C boundary, every C++ symbol stays hidden inside one shared library, and the wheel does not depend on the Python minor version. The cost of a `ctypes` call does not matter at request granularity.

## Considered Options

- **A CPython extension built with nanobind or pybind11, using the stable ABI.** Possible since we require Python 3.12 or later, and the call overhead is lower. It was rejected because it couples the Python build to the Chromium build and leaks C++ types across the boundary.

## Consequences

- The C header is the contract between the two halves and must stay versioned and minimal.
- A Session's engine settings cross the boundary as a versioned JSON buffer, so a new setting does not change the ABI. The native core refuses what it does not know, so that a mistake cannot quietly change what goes on the wire.
- Thread and GIL handoff is done on the Python side; the native core never calls Python on its own network thread except through registered callbacks.
