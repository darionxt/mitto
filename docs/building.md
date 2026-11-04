# Building

The native core is built inside a Chromium checkout ([ADR-0002](adr/0002-build-inside-chromium-tree.md)). That checkout, its tools and its build output live in a **workspace** outside the repository. Terms in **bold** are defined in [CONTEXT.md](../CONTEXT.md).

## Requirements

- A macOS arm64 or Linux x86_64 Host.
- About 31 GiB of free disk space for the workspace, including compiled output (see [Disk usage](#disk-usage)), on a path without spaces.
- `git`, `curl` and `bash`.
- On macOS, a full Xcode installation that includes the macOS SDK. The Command Line Tools are not enough: `gn gen` already looks for the SDK inside Xcode. Chromium's bots build Chromium 157 with the macOS 27.0 SDK (`mac_sdk_official_version` in `build/config/mac/mac_sdk.gni`), so that version is known to work. The native core also builds with Xcode 27.2 and its macOS 27.2 SDK. `xcode-select -p` must point into Xcode.

Linux x86_64 has not been tried yet. Linux-specific steps will be added here when it is.

## Set up the workspace

Choose a directory outside the repository and run the bootstrap script:

```bash
export MITTO_WORKSPACE=/absolute/path/to/mitto-workspace
tools/workspace/bootstrap
```

The script:

1. clones depot_tools into the workspace;
2. writes a `.gclient` file pinned to the tag in [`tools/workspace/chromium_tag`](../tools/workspace/chromium_tag) and runs `gclient sync --no-history`, which fetches `src` and its dependencies at that tag without history, skips those that only Chromium's own bots need and runs Chromium's hooks;
3. links the repository's `native/` directory into the checkout as `//mitto`;
4. copies [`tools/workspace/args.gn`](../tools/workspace/args.gn) into the `out/mitto` build directory and runs `gn gen`, with `//mitto` as the root of the build graph.

The first run downloads about 10 GB and can take hours on a slow connection. The server spends several minutes, sometimes more than ten, preparing `src` before any data arrives, and gclient shows no progress during that time. Arguments to the script, such as `--verbose`, are passed to `gclient sync`.

Running the script again is safe and quick when nothing has changed. After an interrupted run it continues the sync, but a fetch of `src` that did not finish starts over. The script keeps depot_tools' Python environments and gsutil's state in `$MITTO_WORKSPACE/.cache` and turns off depot_tools metrics collection with `DEPOT_TOOLS_METRICS=0`.

The workspace then looks like this:

```text
$MITTO_WORKSPACE/
  .cache/              Python environments and gsutil state for depot_tools
  depot_tools/
  chromium-<tag>/      Chromium checkout of one release
    .gclient           written by the script
    src/               Chromium at the pinned tag
      mitto/           link to the repository's native/
      out/mitto/       build directory for the native core
  compare/             shallow repositories for tools/workspace/compare, if used
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
- `mitto_core_test`, a small C program that fetches one URL through the library and prints the result.

Run the test program, which needs a network connection:

```bash
out/mitto/mitto_core_test
```

It fetches `https://example.com/` unless given another URL. The first line of its output shows the status and the protocol:

```text
status 200 over h2
```

A first build took about 5 minutes on a 10-core Mac, and most of the time goes to `//net` and its dependencies. On macOS arm64 the library is 31 MB, 24 MB once stripped with `strip -x` and 11 MB compressed with gzip. Size has not been worked on yet.

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

## Move to a new Engine Release

Chrome ships a new stable milestone every two weeks, and the Engine Release follows it ([ADR-0005](adr/0005-older-releases-as-data.md)).

1. On [Chromium Dash](https://chromiumdash.appspot.com/releases?platform=Windows), find the newest Windows stable build of the new milestone. Windows is used because the Persona is Windows Chrome.
2. Write that version into `tools/workspace/chromium_tag`.
3. Rerun `tools/workspace/bootstrap`. It creates `$MITTO_WORKSPACE/chromium-<tag>` for the new release, with its own checkout and `out/mitto`.
4. [Build the native core](#build-the-native-core) in the new checkout.
5. Once the new release works, delete the old release's `chromium-<tag>` directory.

Each release has its own checkout because a shallow checkout cannot move to a new release in place: gclient would fetch every branch with full history. Until step 5, the old checkout and its build output stay usable.
