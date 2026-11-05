---
status: accepted
---

# Build the Linux library without system libraries, with one patch to `//net`

The manylinux wheel has to work on machines that have no desktop libraries, and auditwheel's manylinux policies do not allow NSS, NSPR, GIO or D-Bus. So on Linux the native core links only glibc and libgcc. GN arguments turn off GLib, GIO and D-Bus, as Chromium's headless build does. NSS and NSPR have no such path: with `use_nss_server_certs = false`, `//net` does not compile on Linux, because only ChromeOS has a trust store for that case. We carry a patch in `tools/workspace/patches` that makes Linux take ChromeOS's path, which trusts the Chrome Root Store alone.

[ADR-0002](0002-build-inside-chromium-tree.md) warns that build flags that differ from Chrome's can change observable behavior. These do not reach the Wire Fingerprint: at Chrome 157 every BoringSSL and QUICHE object stayed byte-identical, the only `//net` objects that changed are the trust store, the network change notifier and the Linux proxy settings, and the ClientHello did not change.

## Considered Options

- **Keep NSS and bundle it in the wheel.** `auditwheel repair` copies NSS and NSPR into the wheel, but not the modules NSS loads at run time, so on a machine without NSS the process aborts at its first HTTPS request. The wheel's glibc floor also rises to that of the copied NSS: 2.38 on Ubuntu 26.04, against 2.25 for the library itself.
- **Require NSS from the system.** There would be no manylinux wheel. Every process that uses the library would also get an NSS database written to the user's home directory.
- **Replace the trust store function at link time with `--wrap`.** Chromium's source stays untouched, but the build file has to name a mangled C++ symbol and NSS code still compiles. A one-line patch is easier to read and to send upstream.

## Consequences

- On Linux, roots added to an NSS database, or through p11-kit where it replaces `libnssckbi`, are ignored, and requests to servers that rely on them fail. This loses nothing the Persona does, because Chrome on Windows reads the Windows stores, not NSS. An option to add trust anchors is planned instead.
- NSS client certificates and smart cards are not available on Linux, and GNOME's proxy settings would be ignored if mitto ever read the system's proxy settings.
- `tools/workspace/patches` is the only place where mitto changes Chromium's source. A patch there may only make a build configuration possible. It must not change what Chrome's own builds run, or the stack stops being right by construction ([ADR-0001](0001-chromium-network-stack.md)). A patch goes away once upstream has the change.
- Chromium's bots do not build Linux without NSS, so any Engine Release can break this: the patch stops applying, `//net` stops compiling, or a test finds a new system library.
