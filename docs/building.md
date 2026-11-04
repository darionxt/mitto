# Building

The native core is built inside a Chromium checkout ([ADR-0002](adr/0002-build-inside-chromium-tree.md)). That checkout, its tools and its build output live in a **workspace** outside the repository. Terms in **bold** are defined in [CONTEXT.md](../CONTEXT.md).

## Requirements

- A macOS arm64 or Linux x86_64 Host.
- About 31 GiB of free disk space for the workspace, plus room for compiled output (see [Disk usage](#disk-usage)), on a path without spaces.
- `git`, `curl` and `bash`.
- On macOS, a full Xcode installation that includes the macOS SDK. The Command Line Tools are not enough: `gn gen` already looks for the SDK inside Xcode. Chromium's bots build Chromium 157 with the macOS 27.0 SDK (`mac_sdk_official_version` in `build/config/mac/mac_sdk.gni`), so that version is known to work. `gn gen` also succeeds with Xcode 27.2 and its macOS 27.2 SDK, but the native core has not been compiled with it yet. `xcode-select -p` must point into Xcode.

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
3. links the repository's `native/` directory into the checkout as `//mitto`, once that directory exists;
4. copies [`tools/workspace/args.gn`](../tools/workspace/args.gn) into the `out/mitto` build directory and runs `gn gen`.

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
```

To use the Chromium tools by hand, put depot_tools on your `PATH`:

```bash
export PATH="$PATH:$MITTO_WORKSPACE/depot_tools"
```

Change GN arguments in `tools/workspace/args.gn` and rerun the script; the copy in `out/mitto` is overwritten on every run. For experiments, create another build directory under `out/` instead.

## Disk usage

Measured for Chromium 157.0.8103.27 on macOS arm64, after `gn gen` and before any compilation:

| Path under `$MITTO_WORKSPACE` | Size |
| --- | --- |
| `chromium-<tag>/src`, without `out` | 28.6 GiB |
| `chromium-<tag>/src/out/mitto` | 0.5 GiB |
| `depot_tools` | 0.8 GiB |
| `.cache` | 0.6 GiB |
| Total | 30.5 GiB |

Of `src`, 3.6 GiB is the `.git` directories of `src` and its 143 git dependencies. Compiled output in `out/mitto` comes on top of this and has not been measured yet. Each additional release kept in the workspace needs its own `chromium-<tag>` directory.

## Move to a new Engine Release

Chrome ships a new stable milestone every two weeks, and the Engine Release follows it ([ADR-0005](adr/0005-older-releases-as-data.md)).

1. On [Chromium Dash](https://chromiumdash.appspot.com/releases?platform=Windows), find the newest Windows stable build of the new milestone. Windows is used because the Persona is Windows Chrome.
2. Write that version into `tools/workspace/chromium_tag`.
3. Rerun `tools/workspace/bootstrap`. It creates `$MITTO_WORKSPACE/chromium-<tag>` for the new release, with its own checkout and `out/mitto`.
4. Rebuild the native core.
5. Once the new release works, delete the old release's `chromium-<tag>` directory.

Each release has its own checkout because a shallow checkout cannot move to a new release in place: gclient would fetch every branch with full history. Until step 5, the old checkout and its build output stay usable.
