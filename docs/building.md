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

## Build the native core

From `$MITTO_WORKSPACE/chromium-<tag>/src`, with depot_tools on your `PATH`:

```bash
autoninja -C out/mitto mitto
```

This builds two targets from [`native/`](../native), about 4,200 steps in all:

- `mitto_core`, the shared library `out/mitto/libmitto_core.dylib` (`.so` on Linux). It exports only the C API in [`native/include/mitto/mitto.h`](../native/include/mitto/mitto.h).
- `mitto_core_test`, a small C program that creates a Session, fetches one URL through it and prints the result.

Run the test program, which needs a network connection:

```bash
out/mitto/mitto_core_test
```

It fetches `https://example.com/` unless given another URL. The first line of its output shows the status and the protocol:

```text
status 200 over h2
```

A first build took about 5 minutes on a 10-core Mac and on a 16-core Linux machine, and most of the time goes to `//net` and its dependencies. On macOS arm64 the library is 31 MB, 24 MB once stripped with `strip -x` and 11 MB compressed with gzip. On Linux x86_64 it is 120 MB, 27 MB once stripped and 12 MB compressed. Size has not been worked on yet.

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
5. runs `gn gen` and `ninja`, says whether the library is the same as the one the export's slice built, as it is until `native/` changes, and prints the library's path.

Running it again continues where it stopped, and rebuilds what changed in `native/`. It refuses a directory that holds another export of the same release, so a new export needs a new directory. To run the tests against the library:

```bash
export MITTO_NATIVE_LIBRARY=/absolute/path/to/new/directory/chromium-<tag>/src/out/mitto/libmitto_core.so
uv run pytest
```

On macOS the file is `libmitto_core.dylib`.

### Sizes

Measured for Chromium 157.0.8103.27. The archive is the same for both Hosts: 23.5 MiB compressed, 174 MiB unpacked, about 12,300 files.

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

- the **Engine Release** moves;
- a patch in [`tools/workspace/patches`](../tools/workspace/patches) or a value in [`tools/workspace/args.gn`](../tools/workspace/args.gn) changes;
- a new build configuration, such as a sanitizer build, needs files that this one does not;
- `native/` starts to depend on a target outside the current build graph. Until then, `native/` can include any header that a target in the graph declares, because the export keeps those even when nothing includes them yet. A build that needs something else stops with a missing file.

What a build reads differs per Host, and `gn` on Linux knows nothing of macOS's inputs, so each Host contributes a **slice**. The steps take about 15 minutes on two machines that have built the native core before.

1. On each Host, in a workspace [set up by the bootstrap script](#set-up-the-workspace), from the same commit of this repository:

   ```bash
   export MITTO_WORKSPACE=/absolute/path/to/mitto-workspace
   uv run tools/workspace/export.py slice
   ```

   This builds the native core from nothing in `out/export-trace`, with depot_tools off the `PATH`, and then copies what the build used to `$MITTO_WORKSPACE/export/<tag>/<host>`. It removes `out/export-trace` afterwards, and `out/mitto` stays as it was.

2. Copy the macOS slice to the same place on the Linux machine, for example with `rsync -az`.

3. On the Linux machine, merge the slices:

   ```bash
   uv run tools/workspace/export.py merge
   ```

   This writes `$MITTO_WORKSPACE/export/<tag>/mitto-chromium-<tag>.tar.xz` and prints its size, its hash and its contents per directory. The same slices give the same bytes.

4. On each Host, [build from that archive](#build-from-the-source-export) with `--archive` in a new directory, and run the tests. The build must not find its way back to the checkout: keep depot_tools off the `PATH` and `MITTO_WORKSPACE` unset. The command must also say that the library is the same as the one the slice built. A difference means that a file the export left out changed the result without stopping the build, for example through `path_exists()` in GN or `__has_include` in C++. On Linux the comparison covers the whole file. On macOS it covers the file's sections, because the rest holds the absolute paths of the object files.

5. Publish the archive, with the owner's OK:

   ```bash
   gh release create chromium-<tag> "$MITTO_WORKSPACE/export/<tag>/mitto-chromium-<tag>.tar.xz" --prerelease --title "Source export of Chromium <tag>" --notes "Chromium's files for building the native core. See docs/building.md."
   ```

   A later export of the same Engine Release gets a new asset name in the same release, such as `mitto-chromium-<tag>-2.tar.xz`, because commits that name the earlier asset must still build.

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

## Move to a new Engine Release

Chrome ships a new stable milestone every two weeks, and the Engine Release follows it ([ADR-0005](adr/0005-older-releases-as-data.md)).

1. On [Chromium Dash](https://chromiumdash.appspot.com/releases?platform=Windows), find the newest Windows stable build of the new milestone. Windows is used because the Persona is Windows Chrome.
2. Write that version into `tools/workspace/chromium_tag`.
3. Rerun `tools/workspace/bootstrap`. It creates `$MITTO_WORKSPACE/chromium-<tag>` for the new release, with its own checkout and `out/mitto`. If a patch no longer applies, the script stops after the sync. Update the patch against the new checkout and rerun the script. Delete a patch the new release no longer needs.
4. [Build the native core](#build-the-native-core) in the new checkout.
5. [Make the source export](#make-the-source-export) of the new release.
6. Once the new release works, delete the old release's `chromium-<tag>` directory.

Each release has its own checkout because a shallow checkout cannot move to a new release in place: gclient would fetch every branch with full history. Until step 6, the old checkout and its build output stay usable.
