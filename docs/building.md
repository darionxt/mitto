# Building

The native core is built inside a Chromium checkout ([ADR-0002](adr/0002-build-inside-chromium-tree.md)). That checkout, its tools and its build output live in a **workspace** outside the repository. To get the library without a checkout, [build from the source export](#build-from-the-source-export). Terms in **bold** are defined in [CONTEXT.md](../CONTEXT.md).

## Requirements

- A macOS arm64 or Linux x86_64 Host.
- About 31 GiB of free disk space for the workspace, including compiled output (see [Disk usage](#disk-usage)), on a path without spaces.
- `git`, `curl` and `bash`.
- On macOS, a full Xcode installation that includes the macOS SDK. The Command Line Tools are not enough: `gn gen` already looks for the SDK inside Xcode. Chromium's bots build Chromium 157 with the macOS 27.0 SDK (`mac_sdk_official_version` in `build/config/mac/mac_sdk.gni`), so that version is known to work. The native core also builds with Xcode 27.2 and its macOS 27.2 SDK. `xcode-select -p` must point into Xcode.
- On Linux, nothing more. A new Ubuntu 26.04 desktop install only needs `sudo apt-get install git curl`, and other distributions have not been tried. The checkout brings its own compiler, linker and sysroot, so the native core builds without a system C compiler. It also builds without Chromium's `build/install-build-deps.sh`, which installs hundreds of packages for the rest of Chromium.

## Set up the workspace

Choose a directory outside the repository and run the bootstrap script:

```bash
export MITTO_WORKSPACE=/absolute/path/to/mitto-workspace
tools/workspace/bootstrap
```

The script:

1. clones depot_tools into the workspace;
2. writes a `.gclient` file pinned to the tag in [`tools/workspace/chromium_tag`](../tools/workspace/chromium_tag) and runs `gclient sync --no-history`, which fetches `src` and its dependencies at that tag without history, skips those that only Chromium's own bots need and runs Chromium's hooks;
3. applies the patches in [`tools/workspace/patches`](../tools/workspace/patches) to `src`, the only changes to Chromium's source ([ADR-0006](adr/0006-linux-library-without-system-libraries.md));
4. links the repository's `native/` directory into the checkout as `//mitto`;
5. copies [`tools/workspace/args.gn`](../tools/workspace/args.gn) into the `out/mitto` build directory and runs `gn gen`, with `//mitto` as the root of the build graph.

The first run downloads about 10 GB and can take hours on a slow connection. The server spends several minutes, sometimes more than ten, preparing `src` before any data arrives, and gclient shows no progress during that time. Arguments to the script, such as `--verbose`, are passed to `gclient sync`.

Running the script again is safe and quick when nothing has changed. After an interrupted run it continues the sync, but a fetch of `src` that did not finish starts over. The script keeps depot_tools' Python environments and gsutil's state in `$MITTO_WORKSPACE/.cache` and turns off depot_tools metrics collection with `DEPOT_TOOLS_METRICS=0`.

gclient refuses to sync a checkout with local changes, so each run takes the patches out before it syncs and applies them again afterwards. The next build therefore recompiles the patched files. If you have also changed those files in `src`, the script stops until you undo those changes.

The workspace then looks like this:

```text
$MITTO_WORKSPACE/
  .cache/              Python environments and gsutil state for depot_tools
  depot_tools/
  chromium-<tag>/      Chromium checkout of one release
    .gclient           written by the script
    applied-patches/   copies of the patches applied to src
    src/               Chromium at the pinned tag, with the patches applied
      mitto/           link to the repository's native/
      out/mitto/       build directory for the native core
  compare/             shallow repositories for tools/workspace/compare, if used
  export/<tag>/        slices and archive from tools/workspace/export.py, if used
```

To use the Chromium tools by hand, put depot_tools on your `PATH`:

```bash
export PATH="$MITTO_WORKSPACE/depot_tools:$PATH"
```

Change GN arguments in `tools/workspace/args.gn` and rerun the script; the copy in `out/mitto` is overwritten on every run. For experiments, create another build directory under `out/` instead.

Nothing in the Chromium tree refers to `//mitto`, and with these arguments the tree's full build graph does not even generate. So any `gn` command that loads the graph needs the same two switches the script passes, for example:

```bash
gn check out/mitto --root-target=//mitto '--root-pattern=//mitto:*'
```

A build from the [source export](#build-from-the-source-export) runs this check itself, for every `--config`, and CI builds that way. `autoninja` in a checkout does not, so run the check by hand there.

## Build the native core

From `$MITTO_WORKSPACE/chromium-<tag>/src`, with depot_tools on your `PATH`:

```bash
autoninja -C out/mitto mitto
```

This builds two targets from [`native/`](../native), about 4,200 steps in all:

- `mitto_core`, the shared library `out/mitto/libmitto_core.dylib` (`.so` on Linux). It exports only the C API in [`native/include/mitto/mitto.h`](../native/include/mitto/mitto.h).
- `mitto_core_test`, a small C program that drives a request through the C API, the way an outside caller does, and prints the result.

Run the test program, which needs a network connection:

```bash
out/mitto/mitto_core_test
```

It fetches `https://example.com/` unless given another URL, a second argument gives it the settings that `mitto_session_create()` reads, and a third names one of the scenarios at the end of its source, such as `cancel-after-response`. The first line of its output shows the status and the protocol:

```text
status 200 over h2
```

A first build took about 5 minutes on a 10-core Mac and on a 16-core Linux machine, and most of the time goes to `//net` and its dependencies. On macOS arm64 the library is 17 MB, 11 MB once stripped with `strip -x` and 5.4 MB compressed. On Linux x86_64 it is 103 MB, 12 MB once stripped and 6.0 MB compressed. [Size of the library](#size-of-the-library) says where that goes.

On Linux the library needs only glibc and libgcc from the system at run time, as `ldd` shows, and a test checks this. [`tools/workspace/args.gn`](../tools/workspace/args.gn) turns off GLib, GIO, D-Bus, NSS and NSPR, and a patch to `//net` makes that possible for NSS and NSPR. So on Linux the Chrome Root Store is the only source of trust: certificates added to an NSS database are ignored ([ADR-0006](adr/0006-linux-library-without-system-libraries.md)).

## Build from the source export

A hosted runner has no room for a Chromium checkout, so the native core has a second way to build: from the **source export**, an archive of the files of Chromium that its build reads on either **Host**, with the patches already applied. It needs no workspace and no depot_tools.

Run this from the repository root, with a new directory on a path without spaces. It needs about 4.1 GiB there on Linux and 3.2 GiB on macOS, including the build output, as well as `curl` and `tar`. It took about 6 minutes on a 10-core Mac, one of them for the downloads. macOS also needs the same Xcode as a checkout (see [Requirements](#requirements)).

```bash
uv run tools/workspace/export.py build /absolute/path/to/new/directory
```

The command:

1. downloads the archive that [`tools/workspace/source_export.json`](../tools/workspace/source_export.json) names and checks its hash, or uses a local archive given with `--archive`;
2. unpacks it as `chromium-<tag>`, with `src` next to an `export.json` that lists the toolchain;
3. downloads this Host's toolchain from Chromium's own storage and checks each package's hash;
4. links the repository's `native/` in as `//mitto` and copies [`tools/workspace/args.gn`](../tools/workspace/args.gn), so that these two come from the repository and not from the export;
5. runs `gn gen --check`, so that an include that `gn check` refuses stops the build before it compiles anything;
6. runs `ninja`, says whether the library is the same as the one the export's slice built, as it is until `native/` changes, and prints the library's path.

Running it again continues where it stopped, and rebuilds what changed in `native/`. It refuses a directory that holds another export of the same release, so a new export needs a new directory. To run the tests against the library:

```bash
export MITTO_NATIVE_LIBRARY=/absolute/path/to/new/directory/chromium-<tag>/src/out/mitto/libmitto_core.so
uv run pytest
```

On macOS the file is `libmitto_core.dylib`.

### Sizes

Measured for Chromium 157.0.8103.27. The archive is the same for both Hosts: 23.6 MiB compressed, 174 MiB unpacked, about 12,400 files. The export of 157.0.8103.39 is the same size, with the same paths and the same toolchain.

| Toolchain package | Comes from | Linux x86_64: download | unpacked | macOS arm64: download | unpacked |
| --- | --- | --- | --- | --- | --- |
| Clang | Google Storage | 55 MiB | 325 MiB | 47 MiB | 303 MiB |
| Rust | Google Storage | 274 MiB | 1.41 GiB | 246 MiB | 1.28 GiB |
| Linux sysroot | Google Storage | 19 MiB | 221 MiB | | |
| CPython, for build scripts | CIPD | 46 MiB | 152 MiB | 24 MiB | 82 MiB |
| mold, the Linux linker | CIPD | 12 MiB | 41 MiB | | |
| gn | CIPD | 4 MiB | 9 MiB | 1 MiB | 2 MiB |
| ninja | CIPD | under 1 MiB | under 1 MiB | under 1 MiB | under 1 MiB |
| Total | | 410 MiB | 2.14 GiB | 319 MiB | 1.66 GiB |

### Why the toolchain is not an asset

These packages come from Google Storage and CIPD, Chromium's package store, rather than from a release asset of ours. `DEPS` pins each of them for a Chromium release, they are the very packages Chromium's bots build with, and they stay available for old releases. An asset for each Host would fit within GitHub's 2 GiB limit, but it would only copy 300 to 400 MiB per Host and per **Engine Release** for no gain in what is checked: `export.json` records each package's SHA-256, and the build refuses anything else. If a package ever disappears from Chromium's storage, an asset can take over by changing its URL in `export.json`.

### In CI

A job per Host in [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) runs this command, [builds a wheel](#build-a-wheel) from the library and runs `pytest` against the installed wheel. Times are from 14 runs from a cold cache for Chromium 157.0.8103.27, and disk use is from the first. The range is mostly the runners: the same `native/` took 21 and 27 minutes on macOS.

| | Linux x86_64 | macOS arm64 |
| --- | --- | --- |
| Runner | `ubuntu-latest`, 4 cores, 16 GB of memory | `xcode-27`, 3 cores, 7 GB of memory |
| Build | 13 to 24 minutes, under one of them for the downloads | 14 to 28 minutes, under one of them for the downloads |
| Tests | 25 to 33 seconds | 30 to 66 seconds |
| Peak disk use of the job | 4.3 GiB, of which 4.1 GiB is the build's directory | 3.5 GiB, of which 3.2 GiB is the build's directory |
| Disk free when the job starts | 86 GiB | 37 GiB |
| Whole job | 14 to 24 minutes | 15 to 29 minutes |
| Whole job with a cached library | 33 to 41 seconds | 42 to 83 seconds |

The job keeps only the library, in the Actions cache: 28 MB on Linux and 12 MB on macOS, compressed. The key is the runner's name and a hash of `native/` and `tools/workspace/`, which holds the pin of the export. When none of that has changed, the job skips the build. A pull request can read the caches of `main` and its own, so one that changes neither directory uses the library that `main` built. One that changes either builds once, and `main` builds again after the merge.

A second cache keeps the build's directory, about 300 MB per **Host** and build, because what the export compiles to changes only with `tools/workspace/`. Its key is the runner's or the build's name and a hash of that directory. `main` saves it the first time it builds with that hash, and a pull request restores it and rebuilds only `native/`. In one trial, a change to one file in `native/` rebuilt 11 of about 4,000 objects, and the whole job took about 2 minutes on either Host. `main` never restores it, so each merge still builds from nothing and shows a mistake in an incremental build. A pull request that changes `tools/workspace/` also builds from nothing. CIPD packages are unpacked with a fixed file time, because ninja would otherwise rebuild what depends on them.

`xcode-27` is a preview image. Its default is Xcode 27.1 with the macOS 27.0 SDK, the SDK that Chromium's bots use. The Python jobs use it as well, because 9 of 90 jobs on `macos-latest` got no runner within 15 minutes and were cancelled, and none of 36 on `xcode-27` did.

### Sanitizer builds and the fuzz target

On Linux x86_64 the same export also makes the builds that look for memory bugs. Each one has its own directory under `out/`, so one directory can hold them all, and they share about 2.4 GiB of sources and toolchain there. macOS refuses them, because only Linux traces them.

```bash
uv run tools/workspace/export.py build /absolute/path/to/new/directory --config asan
```

| `--config` | Turns on | Also builds | Build directory |
| --- | --- | --- | --- |
| `asan` | AddressSanitizer and LeakSanitizer | | 1.9 GiB |
| `ubsan` | UndefinedBehaviorSanitizer | | 1.7 GiB |
| `fuzz` | AddressSanitizer and libFuzzer | `session_settings_fuzzer`, `request_options_fuzzer`, `cookies_fuzzer`, `socks5_auth_fuzzer`, `client_hints_fuzzer` | 2.2 GiB |

Each of them stops at the first report.

The fuzz targets have a build of their own because Chromium's fuzzing builds define `FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION`, which changes what BoringSSL does, so the library in that build is not the one that ships.

To run the C test program against a local [capture server](capture.md), with `mitto_core_test` from the `asan` or the `ubsan` build:

```bash
export MITTO_CORE_TEST=/absolute/path/to/new/directory/chromium-<tag>/src/out/asan/mitto_core_test
ASAN_OPTIONS=detect_leaks=1 uv run pytest tests/test_core_test_program.py
```

Over each of HTTP/1.1, HTTP/2 and HTTP/3 it makes a request, cancels and releases one before and after its response, destroys a Session and exits the process with one running. It also makes a request that fails on the certificate, tries settings that are refused, fetches a missing intermediate, and destroys a Session and exits the process during such a fetch. A report fails the test, and the test shows it. Chromium's default options leave leak checks off, so the command turns them on. Python cannot load a sanitized library, because the sanitizers' runtime is linked into the program that starts.

To fuzz the code that reads a Session's settings, from the repository root, with new inputs going into a directory of your own. `request_options_fuzzer` does the same for a request's options, `cookies_fuzzer` for cookies, `socks5_auth_fuzzer` for what a SOCKS5 proxy answers and `client_hints_fuzzer` for what a server asks for, with the files of that name:

```bash
mkdir corpus
ASAN_OPTIONS=detect_leaks=1 /absolute/path/to/new/directory/chromium-<tag>/src/out/fuzz/session_settings_fuzzer -max_total_time=60 -dict=native/test/session_settings_fuzzer.dict corpus native/test/session_settings_corpus
```

[`.github/workflows/sanitizers.yml`](../.github/workflows/sanitizers.yml) runs one job per build on `ubuntu-latest`. It runs when a pull request or `main` changes `native/` or `tools/workspace/`, and every day, when the unchanged builds come from the cache and only the fuzzing takes real time. It fuzzes each target for five minutes, or for one minute on a pull request, and keeps any input that crashed one as an artifact of the run. Builds reuse objects from `main` in the same way as in the job above. These jobs are not required checks, because they do not run on every pull request.

Times are from two runs from a cold cache.

| | `asan` | `ubsan` | `fuzz` |
| --- | --- | --- | --- |
| Build | 19 and 26 minutes | 17 and 20 minutes | 24 and 26 minutes |
| Cached, compressed | 59 MiB | 43 MiB | 39 MiB |

## Build a wheel

A wheel of mitto is for one **Host**. Its tag is `py3-none-PLATFORM`, and it holds the native library next to [`src/mitto/_native.py`](../src/mitto/_native.py), where the loader looks for it ([ADR-0003](adr/0003-c-abi-and-ctypes.md)). From the repository root, name a library built [from the source export](#build-from-the-source-export) or [in a checkout](#build-the-native-core):

```bash
MITTO_WHEEL_LIBRARY=/absolute/path/to/libmitto_core.dylib uv build --wheel
```

On Linux the file is `libmitto_core.so`. The build hook in [`tools/wheel.py`](../tools/wheel.py):

1. strips a copy of the library. On macOS it runs `dsymutil` and then `strip -x`, and checks that the copy's code signature still holds, because macOS on arm64 does not load a library whose signature does not. On Linux it runs `objcopy --only-keep-debug`, `strip` and `objcopy --add-gnu-debuglink`;
2. takes the wheel's platform from the stripped library: `macosx_X_0_arm64` from the `minos` of its `LC_BUILD_VERSION` on macOS, and `manylinux_2_X_x86_64` from the highest glibc symbol version it needs on Linux. `auditwheel`, which checks manylinux wheels, has a policy for some versions of glibc only, and a version without one becomes the next version with one;
3. puts it into the wheel as `mitto/libmitto_core.dylib`, or `mitto/libmitto_core.so`;
4. writes what stripping removed into an archive next to the wheel, named after it: `mitto-<version>-py3-none-<platform>.symbols.tar.xz`. It holds the dSYM bundle on macOS and `libmitto_core.so.debug` on Linux.

Every wheel also holds `LICENSE` and the [third-party notices](#third-party-notices) of its library, [`THIRD_PARTY_NOTICES`](../THIRD_PARTY_NOTICES), in its `.dist-info/licenses` directory. `license-files` in `pyproject.toml` names both, and the wheel's `METADATA` lists them in `License-File` lines.

Without `MITTO_WHEEL_LIBRARY` the build stops, so that no wheel without a library can go out: it would install anywhere and fail on its first request. An editable install, which `uv sync` makes, needs no library, because the loader takes the one in `MITTO_NATIVE_LIBRARY`. mitto ships no sdist, because a build from source needs the source export and Chromium's toolchain.

The hook needs Xcode on macOS, and `objcopy` and `strip` from GNU binutils on Linux. On macOS the library only points to the debug information, which stays in the build's object files. For a library whose build directory is gone, such as one restored from CI's cache, `dsymutil` warns about each missing file, and the symbols keep function names but lose line numbers.

To turn an address in the library, such as one from a crash report, back into a function name, unpack the symbols archive and run one of these, on macOS and on Linux, with the address's offset from the start of the library in place of `0x3c44`:

```bash
atos -o libmitto_core.dylib.dSYM -l 0 0x3c44
addr2line -f -C -e libmitto_core.so.debug 0x3c44
```

Measured for Chromium 157.0.8103.39, in MB of 1,000,000 bytes:

| | Linux x86_64 | macOS arm64 |
| --- | --- | --- |
| Tag | `py3-none-manylinux_2_26_x86_64` | `py3-none-macosx_13_0_arm64` |
| Wheel | 6.1 MB | 5.5 MB |
| Library, installed | 12.3 MB | 10.8 MB |
| Symbols archive | 12.8 MB | 11.5 MB, or under 1 MB without the object files |

The tag follows from Chromium's build. Chromium 157 sets `mac_deployment_target` to 13.0 in `build/config/mac/mac_sdk.gni`. Its Linux sysroot is Debian bullseye, and `build/linux/sysroot_scripts/reversion_glibc.py` rewrites the sysroot's glibc so that no symbol needs a version above 2.26. The library needs 2.25, for `getentropy` and `getrandom`, and auditwheel has no policy for 2.25, so the tag says 2.26. [`tests/test_wheel.py`](../tests/test_wheel.py) keeps the tag equal to what the library needs: what `auditwheel show` says on Linux, and the `minos` that `otool -l` shows on macOS.

To check a wheel, install it into a new environment and run the tests there, with `MITTO_WHEEL` naming the wheel, `MITTO_HOST_NOTICES` naming what the [notices script](#third-party-notices) prints for the library's build, and `MITTO_NATIVE_LIBRARY` unset. Print the notices first, because `uv run` would install the project into the new environment:

```bash
uv run python -m tools.workspace.notices host /absolute/path/to/chromium-<tag>/src/out/mitto > dist/host-notices.txt
export UV_PROJECT_ENVIRONMENT=/absolute/path/to/new/environment
uv sync --no-install-project
uv pip install --python "$UV_PROJECT_ENVIRONMENT/bin/python" dist/mitto-*.whl
MITTO_WHEEL="$(ls "$PWD"/dist/mitto-*.whl)" MITTO_HOST_NOTICES="$PWD/dist/host-notices.txt" "$UV_PROJECT_ENVIRONMENT/bin/python" -m pytest
```

The suite then runs against the installed package, and `MITTO_WHEEL` adds the tests of the wheel itself: that the installed package loads the wheel's library, that the tag is what the library needs, that the symbols archive belongs to the library and names a function that the stripped library does not, and that the wheel holds the notice of every project that its library links on this Host.

### Third-party notices

The library links Chromium and about 80 projects that Chromium uses, such as BoringSSL, QUICHE, ICU, zlib, Brotli, zstd, Abseil, Perfetto, Rust crates and the Rust standard library. Their licences ask that a binary carries their notices, so [`THIRD_PARTY_NOTICES`](../THIRD_PARTY_NOTICES) holds an entry for each project, with the text of its licence, and every wheel carries that file.

[`tools/workspace/notices.py`](../tools/workspace/notices.py) writes it from the library's build graph, as Chromium's `tools/licenses/licenses.py` does for Chrome. The source export does not hold that script, and the script would also count the build's tools:

1. `gn desc` describes every target of the build graph, in a directory of its own with the build directory's `args.gn`, so that the build directory stays as it is.
2. From `//mitto:mitto_core`, it follows the dependencies whose code a shared library links: static libraries, source sets, Rust libraries and groups. An action, an executable and a Rust procedural macro only run during the build, so it stops there. protoc, the program that generates the HSTS preload list and serde's derive macros need no notice.
3. A linked target's code lies in the directories of its sources, or in the target's own directory when all its sources are generated. A Rust crate's code counts as its target's directory, which holds its `README.chromium`.
4. A directory belongs to the nearest project: a directory under a `third_party` directory, or a crate's `third_party/rust/<crate>/<epoch>`, that holds a `README.chromium`. One without, such as a project inside another project, is the nearest directory with licence files, and one with neither belongs to the project around it. Code outside any project is Chromium's, and the library's own code is under `LICENSE`.
5. The `README.chromium` gives each project's name, URL, licence and licence files, with `COPYING` when the files it names do not exist. A dependency marked `Shipped: no` gets no entry: Chromium marks code whose licence another entry already carries that way, such as the part of ICU that `//base` copies. The Rust standard library has no `README.chromium`, and its licence texts come from the Rust toolchain. A linked project without a licence file stops the script.

The Hosts link slightly different projects: only Linux links compiler-rt, glog's symbolizer and xdg-user-dirs. gn on one Host cannot describe the other Host's build, so each entry says which Hosts link its project, and each Host writes its own part, from a build of the [source export](#build-from-the-source-export) or of a [checkout](#build-the-native-core):

```bash
uv run python -m tools.workspace.notices update /absolute/path/to/chromium-<tag>/src/out/mitto
```

It says which entries changed, and drops an entry that no Host links any longer. The native job of each Host in CI keeps that Host's part as the artifact `notices-<runner>` of its run, so one machine can take the other Host's part from there:

```bash
gh run download <run-id> --name notices-ubuntu-latest --dir /absolute/path/to/new/directory
uv run python -m tools.workspace.notices merge /absolute/path/to/new/directory/host-notices.txt
```

Update both parts whenever what the library links may change: when the **Engine Release** moves, in [the release routine](releasing.md#3-make-the-source-export), when `native/BUILD.gn` changes its dependencies, and when `tools/workspace/args.gn` changes. Until a Host's part is right, the test of the installed wheel fails on that Host and names each project that the notices get wrong: one they miss, one they say is linked but is not, and one whose licence text changed.

Measured for Chromium 157.0.8103.27, the notices are 0.6 MB, and they add 0.1 MB to a wheel.

### In CI

The native job of each Host builds a wheel from the library it built or restored, and keeps the wheel and its symbols archive as an artifact of the run. When it builds the library, it also prints this Host's part of the [third-party notices](#third-party-notices) from the same build, and keeps it next to the library in the cache and as an artifact. It installs the wheel into a new environment and runs the tests as above. This adds about 10 seconds on either Host.

Once, the wheels also passed their tests on the oldest system that their tags claim and that GitHub offers. The Linux wheel ran in an `amazonlinux:2` container on `ubuntu-latest`, with glibc 2.26. The macOS wheel ran on `macos-15`, with macOS 15.8: GitHub no longer offers `macos-14`, and never had an arm64 runner with macOS 13.

## Size of the library

The roadmap aims at about 5 MB per wheel. A wheel holds the library, stripped as the [wheel's build](#build-a-wheel) strips it and compressed with deflate, beside the Python package and the [third-party notices](#third-party-notices), which add 0.1 MB. Measured for Chromium 157.0.8103.39, in MB of 1,000,000 bytes, before and after [#123](https://github.com/darionxt/mitto/issues/123):

| | Linux x86_64 | macOS arm64 |
| --- | --- | --- |
| Library, stripped, before | 26.94 MB | 23.86 MB |
| Library, stripped, now | 12.32 MB | 10.84 MB |
| Wheel, before | 12.3 MB | 11.2 MB |
| Wheel, now | 6.14 MB | 5.55 MB |

### What takes the space

The linker writes a map of the library when its link command, which `ninja -C out/mitto -t commands mitto_core | tail -1` prints, gets `-Wl,-map,map.txt` on macOS or `-Wl,-Map=map.txt` on Linux as well. The map names the object file of each symbol, so each byte counts for the project of its object file. Only the sections that stripping keeps count here. The last row is what the linker writes itself: on Linux the unwind tables, the relocations and the strings that it merges from all objects, and on macOS the loader's tables and the code signature.

| Part | Linux before | Linux now | macOS before | macOS now |
| --- | --- | --- | --- | --- |
| ICU's data | 10.82 | 0.32 | 10.82 | 0.32 |
| `//net`, without QUICHE | 5.29 | 4.47 | 5.18 | 4.43 |
| of which the HSTS preload list | 0.78 | 0.78 | 0.79 | 0.79 |
| of which the Chrome Root Store | 0.38 | 0.38 | 0.38 | 0.38 |
| QUICHE | 1.08 | 1.08 | 1.11 | 1.11 |
| BoringSSL | 1.15 | 1.15 | 0.93 | 0.92 |
| ICU's code | 1.10 | 0.63 | 1.09 | 0.63 |
| SQLite and `//sql` | 0.92 | 0 | 0.92 | 0 |
| `//base` | 0.86 | 0.82 | 0.82 | 0.78 |
| zlib, Brotli and zstd | 0.76 | 0.76 | 0.66 | 0.66 |
| Rust's standard library and crates | 0.46 | 0.46 | 0.43 | 0.43 |
| simdutf | 0.44 | 0.44 | 0.15 | 0.15 |
| Abseil | 0.23 | 0.23 | 0.21 | 0.21 |
| libc++ | 0.21 | 0.21 | 0.32 | 0.32 |
| RE2 | 0.18 | 0.18 | 0.18 | 0.18 |
| mitto's own code | 0.16 | 0.16 | 0.15 | 0.15 |
| `//components`, for device bound sessions | 0.13 | 0 | 0.13 | 0 |
| Everything else: protobuf, `//url`, Perfetto, PartitionAlloc, `//crypto` | 0.33 | 0.31 | 0.37 | 0.30 |
| What the linker writes itself | 2.82 | 1.10 | 0.39 | 0.25 |
| Total | 26.94 | 12.32 | 23.86 | 10.84 |

By section, before and now:

| Linux x86_64 | Before | Now |
| --- | --- | --- |
| `.text` | 11.10 | 8.78 |
| `.rodata`, which holds ICU's data | 12.92 | 2.34 |
| `.rodata.str1.1`, the strings that the linker merges | 0.40 | 0.34 |
| `.eh_frame` and `.eh_frame_hdr` | 1.55 | 0.03 |
| `.rela.dyn` | 0.55 | 0.46 |
| `.data.rel.ro` | 0.27 | 0.24 |
| Other sections | 0.15 | 0.13 |

| macOS arm64 | Before | Now |
| --- | --- | --- |
| `__TEXT,__text` | 9.91 | 7.71 |
| `__TEXT,__const`, which holds ICU's data | 12.85 | 2.27 |
| `__TEXT,__cstring` | 0.40 | 0.34 |
| `__DATA_CONST,__const` | 0.30 | 0.26 |
| `__LINKEDIT`, the loader's tables and the code signature | 0.27 | 0.15 |
| Other sections | 0.13 | 0.11 |

### What each change saves

Each change was measured on its own, on top of the build before #123, as the stripped library and as the library compressed, which is what a wheel holds. Identical code folding, `--icf=all`, was already on: Chromium turns it on for every build with lld or mold that is neither a debug nor a sanitizer build.

| Change | Linux: stripped, compressed | macOS: stripped, compressed | Taken |
| --- | --- | --- | --- |
| [ICU's data](#icus-data) cut to the items the library can read, which needs the next row | −10.50, −4.45 | −10.50, −4.45 | Yes |
| `enable_device_bound_sessions` and `enable_disk_cache_sql_backend` off | −2.88, −1.35 | −2.44, −1.17 | Yes |
| `exclude_unwind_tables` | −1.54, −0.52 | 0, 0 | Yes |
| `optimize_for_size` | −2.16, −0.94 | −1.86, −0.86 | No, see [what is left](#what-is-left) |
| `is_official_build`, with `chrome_pgo_phase = 0` | −3.01, −1.26 | −1.37, −0.65 | No, see [what is left](#what-is-left) |
| `is_official_build` without ThinLTO | −2.54, −1.03 | −0.86, −0.41 | No |
| `use_thin_lto`, without the optimizations of an official build | −0.51, −0.29 | −0.51, −0.23 | No, see [what is left](#what-is-left) |
| lld instead of mold | −0.05, −0.07 | | No |
| All that is taken, together | −14.62, −6.21 | −13.02, −5.70 | |

The ICU row is measured on the data alone, because without the row below it the library could read much more of it.

What each taken change does besides size:

- **`enable_device_bound_sessions = false` and `enable_disk_cache_sql_backend = false`.** `//net` makes neither unless its embedder asks. Its builder makes the device bound session service only after `set_has_device_bound_session_service(true)`, and the SQL backend of the HTTP cache only for the cache type of that name, while a **Session** asks for neither: it uses the blockfile backend. Behind both flags, `//net` only leaves out the calls into these two, which find nothing there today. Together they linked SQLite and `//sql`, and SQLite's ICU extension linked ICU's collation, regular expressions, break iterators and resource bundles, and with them the reason to keep most of ICU's data. Chrome on Windows has both. If mitto ever offers device bound sessions, the first flag comes back, and SQLite and that data with it.
- **ICU's data**: see [below](#icus-data). The library reads the same bytes it read before, from a smaller package.
- **`exclude_unwind_tables = true`.** As in Chrome's releases, the compiler leaves out the tables that unwind a stack at run time. Chromium's code throws no exceptions, and Rust's panics abort. What the library loses is the stack in a crash message of its own, which then stops at the first frame. A debugger still unwinds with `.debug_frame`, which the compiler writes instead and which the symbols archive keeps. The sanitizer builds keep the tables, because their reports need them.

### ICU's data

`//url` turns a hostname that is not ASCII into ASCII by UTS 46, with ICU, and ICU reads the rules from its data. Chromium links all of ICU's data into a program, 10.82 MB in 2,360 items, for the many parts of Chrome that use ICU. ICU reads an item only when one of its functions asks for that item by name. Of those functions, the library links only those in the table below, and each asks for fixed names, so the library holds those 9 items and no other, 0.32 MB. [`native/make_icu_data.py`](../native/make_icu_data.py) copies them unchanged from Chromium's data file while the library builds, into a package of ICU's own format, under the symbol through which ICU finds its data. [`tools/workspace/args.gn`](../tools/workspace/args.gn) then has ICU's own build leave its data out.

| Items | Read by | For |
| --- | --- | --- |
| `uts46.nrm`, `nfkc.nrm`, `nfkc_cf.nrm` | ICU's loader of normalizations, which knows these by name | `//url`'s IDNA reads `uts46`, and the NFKC of `//base` reads `nfkc` |
| `cnvalias.icu`, `windows-1252-html.cnv` | `ucnv_open()` | The realm of a Basic or Digest challenge, which `//net` reads as ISO-8859-1. Chromium's data makes that name stand for windows-1252, as HTML does |
| `keyTypeData.res`, `timezoneTypes.res`, `metadata.res`, `langInfo.res` | ICU's code for locales | The default locale, which ICU canonicalizes when NTLM writes a user name in upper case. `keyTypeData` points into `timezoneTypes` |

The rest is for collation, break iteration, regular expressions, formatting, the names of languages, regions and currencies, time zones, transliteration and character sets other than ISO-8859-1 and UTF-8, and the library links no function that reads it.

The library itself shows this. Of ICU's entry points to its data, `udata_open()`, `udata_openChoice()`, `ures_open()`, `ures_openDirect()`, `ures_openNoDefault()` and `ucnv_open()`, the linker keeps only three, on both Hosts, and `objdump -d` of the unstripped library shows every call to them:

| Entry point | Called by |
| --- | --- |
| `ucnv_open()` | `base::CodepageToUTF16()`, which only `//net`'s Basic and Digest handlers reach, with ISO-8859-1 |
| `udata_openChoice()` | ICU's loader of normalizations; `haveAliasData()` and `createConverterFromFile()`, which `ucnv_open()` calls; `res_load()`, which `ures_openDirect()` calls |
| `ures_openDirect()` | ICU's `AliasReplacer` for `metadata`, `LikelySubtagsData` for `langInfo`, and the keyword table of `uloc_keytype.cpp` for `keyTypeData` |

`-Wl,-why_live,<symbol>` on the link, such as `-Wl,-why_live,_ucol_open_78`, says which caller keeps a function, which is how SQLite's ICU extension turned up. [`tests/test_hostnames.py`](../tests/test_hostnames.py) holds what IDNA gives for 46 hostnames, as recorded from a library with all of ICU's data, and the tests of proxies answer Basic and Digest challenges. A test run is how the first version of this list was found wrong: it lacked the windows-1252 table, and every challenge failed.

What the library links can change with any **Engine Release**, so [the release routine](releasing.md#3-make-the-source-export) checks this list again.

### What is left

Now the wheels are 6.14 MB on Linux and 5.55 MB on macOS. Of the stripped library, `//net` and QUICHE are 5.55 MB on Linux and 5.54 MB on macOS, BoringSSL 1.15 and 0.92 MB, and `//base`, zlib, Brotli and zstd, ICU's code, Rust, simdutf and libc++ most of the rest. `//net` holds data that decides what a Session does: the HSTS preload list, 0.78 MB, and the Chrome Root Store, 0.38 MB. These further cuts were measured and not taken, and each is the owner's choice:

| Cut | Saves per wheel | Costs |
| --- | --- | --- |
| `optimize_for_size = true` | About 0.9 MB on each Host | Speed: the compiler optimizes C++ with `-Os` instead of `-O2`, and Rust for size, as Chrome does only on Android. Nothing changes on the wire |
| `use_thin_lto = true`, with `use_mold = false` for Linux | About 0.25 MB on each Host | On macOS the linker writes ThinLTO's object files into a directory that it removes afterwards, so the symbols archive keeps function names but loses line numbers. `use_mold` exists on Linux only, so gn warns about it on macOS |
| `is_official_build = true` with `chrome_pgo_phase = 0`, `is_cfi = false`, `enable_dsyms = false` and `enable_stripping = false`, which brings ThinLTO with its optimizations | About 0.45 MB on Linux and 0.4 MB on macOS beyond the cut above it | A new source export: an official build reads `build/util/LASTCHANGE`, which only a checkout has. A failed `CHECK()` then crashes without writing its message, and a failed check that is not fatal yet, such as `DUMP_WILL_BE_CHECK()`, only logs, as in Chrome, where other builds crash. The build's date becomes its commit's time, as in Chrome, where other builds round it down to the first Sunday of the month. `//net` stops applying the HSTS preload list and its pins 70 days after that date, so this moves mitto's day onto Chrome's |

Other parts of `//net` that a build flag leaves out are small on macOS: Reporting with Network Error Logging 0.14 MB, mDNS 0.04 MB, WebSockets 0.02 MB and Kerberos 0.01 MB, before compression. Each would need the same reading of what a Session reaches as the two flags above, and none was tried. The HSTS preload list and the Chrome Root Store decide what a Session does, so they stay.

## Work on the Python package

The Python package is in [`src/mitto`](../src/mitto). Working on it needs [uv](https://docs.astral.sh/uv/) but no Chromium checkout. This command, run from the repository root, creates `.venv` with Python 3.12, the lowest version mitto supports, and the locked development tools:

```bash
uv sync
```

Run the checks:

```bash
uv run ruff check
uv run ruff format --check
uv run pyright
uv run pytest
```

To run the tests on another Python version:

```bash
uv run --python 3.14 --isolated pytest
```

Tests that call the real native core are skipped when there is no native library. They need no network connection, because they only talk to a local server. To run them against a local build, name the library in `MITTO_NATIVE_LIBRARY`:

```bash
export MITTO_NATIVE_LIBRARY="$MITTO_WORKSPACE/chromium-<tag>/src/out/mitto/libmitto_core.dylib"
uv run pytest
```

On Linux the file is `libmitto_core.so`.

Those tests include the **Parity Checks**, described in [parity.md](parity.md).

Tests of how the loader rejects a library compile small stub libraries with `cc`, and are skipped when there is no C compiler. A new Ubuntu install has none, and the native core's build does not need one. Install `gcc` to run these tests.

To check the whole path from Python through the native core to a real server, with `MITTO_NATIVE_LIBRARY` set as above and a network connection:

```bash
uv run python -c "import mitto; print(mitto.get('https://example.com/').status)"
```

It prints `200`.

The loader in [`src/mitto/_native.py`](../src/mitto/_native.py) uses `MITTO_NATIVE_LIBRARY` if it is set, and otherwise `libmitto_core.dylib` (`.so` on Linux) next to that file in the installed package. If the variable names something that is not a file, loading fails rather than falling back to the installed library. The loader refuses a library whose ABI version differs from its own `ABI_VERSION`, and a test keeps that constant equal to `MITTO_ABI_VERSION` in the C header.

## Disk usage

Measured for Chromium 157.0.8103.27 on macOS arm64, after building the native core:

| Path under `$MITTO_WORKSPACE` | Size |
| --- | --- |
| `chromium-<tag>/src`, without `out` | 28.6 GiB |
| `chromium-<tag>/src/out/mitto` | 1.0 GiB |
| `depot_tools` | 0.8 GiB |
| `.cache` | 0.6 GiB |
| Total | 31.0 GiB |

Of `src`, 3.6 GiB is the `.git` directories of `src` and its 143 git dependencies. Each additional release kept in the workspace needs its own `chromium-<tag>` directory.

On Linux x86_64 the same workspace takes 29.7 GiB, of which 1.4 GiB is `out/mitto`.

## Compare two releases

Older Chrome Releases are reproduced as **Persona** data and never built ([ADR-0005](adr/0005-older-releases-as-data.md)), so they only need comparing. `tools/workspace/compare` shows where their TLS, HTTP/2 and QUIC differences may lie, without a full checkout. **Baselines** recorded from real Chrome remain the ground truth.

```bash
export MITTO_WORKSPACE=/absolute/path/to/mitto-workspace
tools/workspace/compare 156.0.8078.80 157.0.8103.27
```

The script:

1. fetches each tag of `chromium/src` at depth 1 into `$MITTO_WORKSPACE/compare/src`, which is independent of the `chromium-<tag>` checkouts;
2. prints the size of the whole change in `src` and a per-file summary of its default network paths, or of the paths given after the two tags;
3. reads the BoringSSL and QUICHE revisions and URLs from each tag's `DEPS`, fetches those commits into `compare/boringssl` and `compare/quiche`, and prints the same for each whole repository, because TLS and QUIC code lives there and a diff of `src` alone misses it.

Diffs go to stdout, and progress and warnings go to stderr. Tags and commits the repositories already hold are not fetched again, so a rerun needs no network.

These costs come on top of the [disk usage](#disk-usage) above. They were measured on macOS arm64 with 156.0.8078.80 and 157.0.8103.27, and the times depend on the connection:

| Step | Time | Disk |
| --- | --- | --- |
| First tag | about 8.5 minutes, most of it the server preparing the pack | 1.35 GiB |
| Tag from another milestone | about 2 minutes | about 200 MiB |
| Tag from the same milestone | seconds | under 1 MiB |
| BoringSSL and QUICHE, two commits each | seconds | under 100 MiB |
| Diff | about 15 seconds | none |

The repositories keep every tag and commit they fetch. Delete `$MITTO_WORKSPACE/compare` to free the space; the next run fetches what it needs again.

## Make the source export

[`tools/workspace/export.py`](../tools/workspace/export.py) makes the source export by hand, on machines that hold a workspace. Make a new one when:

- the **Engine Release** moves, in [the release routine](releasing.md);
- a patch in [`tools/workspace/patches`](../tools/workspace/patches) or a value in [`tools/workspace/args.gn`](../tools/workspace/args.gn) changes;
- a new build configuration, such as another sanitizer, needs files that this one does not;
- `native/` starts to depend on a target outside the current build graph. Until then, `native/` can include any header that a target in the graph declares, because the export keeps those even when nothing includes them yet. A build that needs something else stops with a missing file.

What a build reads differs per Host, and `gn` on Linux knows nothing of macOS's inputs, so each Host contributes a **slice**. A slice covers every build configuration that the Host has: only the plain one on macOS, and also `asan`, `ubsan` and `fuzz` on Linux. The steps take about 45 minutes on two machines that have built the native core before.

1. On each Host, in a workspace [set up by the bootstrap script](#set-up-the-workspace), from the same commit of this repository:

   ```bash
   export MITTO_WORKSPACE=/absolute/path/to/mitto-workspace
   uv run tools/workspace/export.py slice
   ```

   This builds the native core from nothing in `out/export-trace-<config>`, once per build configuration, with depot_tools off the `PATH`, and then copies what those builds used to `$MITTO_WORKSPACE/export/<tag>/<host>`. It removes each build directory afterwards, and `out/mitto` stays as it was.

2. Copy the macOS slice to the same place on the Linux machine, for example with `rsync -az`.

3. On the Linux machine, merge the slices:

   ```bash
   uv run tools/workspace/export.py merge
   ```

   This writes `$MITTO_WORKSPACE/export/<tag>/mitto-chromium-<tag>.tar.xz` and prints its size, its hash and its contents per directory. The same slices give the same bytes.

4. On each Host, [build from that archive](#build-from-the-source-export) with `--archive` in a new directory, once with each `--config` that the Host has, and run the tests. The build must not find its way back to the checkout: keep depot_tools off the `PATH` and `MITTO_WORKSPACE` unset. The command must also say each time that the library is the same as the one the slice built. A difference means that a file the export left out changed the result without stopping the build, for example through `path_exists()` in GN or `__has_include` in C++. On Linux the comparison covers the whole file. On macOS it covers the file's sections, because the rest holds the absolute paths of the object files.

5. Publish the archive, with the owner's OK:

   ```bash
   gh release create chromium-<tag> "$MITTO_WORKSPACE/export/<tag>/mitto-chromium-<tag>.tar.xz" --prerelease --title "Source export of Chromium <tag>" --notes "Chromium's files for building the native core. See docs/building.md."
   ```

   A later export of the same Engine Release gets a new asset name in the same release, because commits that name the earlier asset must still build. Copy the archive to a name such as `mitto-chromium-<tag>-2.tar.xz` and add it with `gh release upload chromium-<tag>`.

6. Write the asset's URL and SHA-256 into [`tools/workspace/source_export.json`](../tools/workspace/source_export.json).

A slice holds:

- every file that `gn` read, and every source, header, input and script that a target in the build graph declares;
- every file in `ninja -t deps` and in the depfiles that build steps left behind;
- every file of the source tree that a build step's command line names, such as a compiler's warning suppression list, which no depfile lists;
- every file below a directory that compilers search with `-isystem`, because `-MMD` leaves those headers out of its depfiles;
- every file that a build script's Python opened or got as an argument, recorded through Python's audit events, which see this on both Hosts without root;
- `DEPS`, and `build/modules/linux/BUILD.gn`, which `gn` only checks with `path_exists()`;
- the licence files that apply to all of the above: `LICENSE`, `COPYING`, `NOTICE`, `AUTHORS` and similar names in each directory on the way up to a kept file, and `README.chromium` with the file its `License File:` field names.

It leaves out the build output, `//mitto` and the toolchain. It stops if it would keep a prebuilt program, because that belongs in the toolchain list at the top of the script.

The merge stops if a path differs between slices, other than `build/config/gclient_args.gni`, which gclient writes per Host and whose `checkout_*` values decide nothing in this build graph. It also stops if a third-party project in the export has no licence file. It reads the toolchain's pins from `DEPS`, and resolves those in CIPD to a hash with depot_tools.
