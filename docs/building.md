# Building

The native core is built inside a Chromium checkout ([ADR-0002](adr/0002-build-inside-chromium-tree.md)). That checkout, its tools and its build output live in a **workspace** outside the repository. To get the library without a checkout, [build from the source export](#build-from-the-source-export). Terms in **bold** are defined in [CONTEXT.md](../CONTEXT.md).

## Requirements

- A macOS arm64, Linux x86_64 or Windows x64 Host.
- About 31 GiB of free disk space for the workspace, including compiled output (see [Disk usage](#disk-usage)), on a path without spaces.
- `git`, and [uv](https://docs.astral.sh/uv/) to run the set-up script with Python 3.12 or later. The script needs only Python's standard library. On macOS and Linux, depot_tools also needs `curl` and `bash`.
- On macOS, a full Xcode installation that includes the macOS SDK. The Command Line Tools are not enough: `gn gen` already looks for the SDK inside Xcode. Chromium's bots build Chromium 157 with the macOS 27.0 SDK (`mac_sdk_official_version` in `build/config/mac/mac_sdk.gni`), so that version is known to work. The native core also builds with Xcode 27.2 and its macOS 27.2 SDK. `xcode-select -p` must point into Xcode.
- On Linux, nothing more. A new Ubuntu 26.04 desktop install only needs `sudo apt-get install git curl`, and other distributions have not been tried. The checkout brings its own compiler, linker and sysroot, so the native core builds without a system C compiler. It also builds without Chromium's `build/install-build-deps.sh`, which installs hundreds of packages for the rest of Chromium.
- On Windows, what Chromium's own [instructions for Windows](https://chromium.googlesource.com/chromium/src/+/refs/tags/157.0.8103.39/docs/windows_build_instructions.md) ask for. The checkout brings its own compiler and linker, clang-cl and lld-link, but they take Windows' headers, libraries and some tools from the machine:
  - Visual Studio 2026, version 18.0 or later; Community is enough. It needs "Desktop development with C++" and that workload's ATL/MFC component. From the command line, its installer adds them with `--add Microsoft.VisualStudio.Workload.NativeDesktop --add Microsoft.VisualStudio.Component.VC.ATLMFC --includeRecommended`. Version 18.11 took 7.6 GiB this way, and its installer kept another 2.8 GiB of packages.
  - The Windows SDK 10.0.28000.2270, with its Debugging Tools for Windows, about 4 GiB with all its features. Its installer is on Microsoft's [Windows SDK downloads](https://learn.microsoft.com/en-us/windows/apps/windows-sdk/downloads) page. Chromium 157's `build/vs_toolchain.py` asks for `10.0.28000.0`, the directory that every build of that SDK installs into.
  - [Git for Windows](https://gitforwindows.org/), with `git` on `PATH`. depot_tools no longer brings its own git on Windows: it looks on `PATH` for a git inside a directory named `Git`. Its portable MinGit works as well, unpacked into a directory of that name.
  - An NTFS disk and a short path for the workspace, such as `C:\ws`. The longest path of the checkout of Chromium 157.0.8103.39 lies 228 characters below the workspace, so a workspace path of up to 31 characters keeps every path within Windows' limit of 260. Without Windows' own setting for long paths, which applies to every program, only git can go past that limit, because the script lets it.

## Set up the workspace

Choose a directory outside the repository and run the bootstrap script from the repository root:

```bash
export MITTO_WORKSPACE=/absolute/path/to/mitto-workspace
uv run tools/workspace/bootstrap.py
```

On Windows, in PowerShell:

```powershell
$env:MITTO_WORKSPACE = 'C:\ws'
uv run tools/workspace/bootstrap.py
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

Every git command that the script runs, those of depot_tools and gclient included, gets `core.autocrlf=false` and `core.eol=lf`, a Git LFS filter that does nothing, and on Windows also `core.longpaths=true`. They come through git's `GIT_CONFIG_COUNT` variables in the environment, which take precedence over the user's own git configuration and leave it as it is. So each file of the checkout has the same bytes on every **Host**, as the [source export](#make-the-source-export) needs: Git for Windows would otherwise write CRLF line endings into the files that a `.gitattributes` marks as text. Chromium's git server has no Git LFS, so a file that a dependency keeps in LFS stays the small pointer file that git holds. MinGit's own configuration requires an LFS filter without bringing git-lfs, which would stop the checkout. A `gclient sync` run by hand on Windows lacks these settings, so rerun the script instead. On Windows, depot_tools also prints a warning that it recommends changes to the user's global git configuration. The checkout does not need them, and the script leaves that configuration alone.

On Windows the script differs in three more ways. `//mitto` is a directory junction, because a symbolic link needs a privilege that most users lack. depot_tools' commands are its `.bat` wrappers, which run in cmd.exe whichever shell starts the script, as Chromium's instructions ask. And everything it runs gets `DEPOT_TOOLS_WIN_TOOLCHAIN=0`, so that depot_tools uses the Visual Studio installed on the machine rather than a packaged copy that only Google's own machines can download.

The workspace then looks like this:

```text
$MITTO_WORKSPACE/
  .cache/              Python environments and gsutil state for depot_tools
  depot_tools/
  chromium-<tag>/      Chromium checkout of one release
    .gclient           written by the script
    applied-patches/   copies of the patches applied to src
    src/               Chromium at the pinned tag, with the patches applied
      mitto/           link to the repository's native/, a junction on Windows
      out/mitto/       build directory for the native core
  compare/             shallow repositories for tools/workspace/compare, if used
  export/<tag>/        slices and archive from tools/workspace/export.py, if used
```

To use the Chromium tools by hand, put depot_tools on your `PATH`:

```bash
export PATH="$MITTO_WORKSPACE/depot_tools:$PATH"
```

On Windows, in PowerShell, also tell depot_tools to use the installed Visual Studio:

```powershell
$env:PATH = "$env:MITTO_WORKSPACE\depot_tools;$env:PATH"
$env:DEPOT_TOOLS_WIN_TOOLCHAIN = '0'
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

On Windows, `autoninja` picks Siso, Chromium's other build tool, which does not follow the junction that `//mitto` is there and stops before it builds anything. Run the checkout's own ninja instead, from the same directory:

```powershell
third_party\ninja\ninja.exe -C out\mitto mitto
```

depot_tools' `ninja` command refuses a build directory where Siso has run, and this one does not.

This builds three targets from [`native/`](../native), about 4,200 steps in all:

- `mitto_core`, the shared library `out/mitto/libmitto_core.dylib`, `libmitto_core.so` on Linux and `mitto_core.dll` on Windows. It exports only the C API in [`native/include/mitto/mitto.h`](../native/include/mitto/mitto.h), and on Windows also `GetHandleVerifier`, which `//base` marks `__declspec(dllexport)` in `base/win/scoped_handle_verifier.cc`. A linker cannot take that away, and it clashes with nothing, because Windows looks up an import in the DLL that the import names. `//base` only looks for it in the program that started the process.
- `mitto_core_test`, a small C program that drives a request through the C API, the way an outside caller does, and prints the result. Its scenarios of Pages need the Page program beside the library ([runtime.md](runtime.md#tests)).
- `html_tokenizer_test`, which runs html5lib's tokenizer tests against the **Runtime**'s HTML tokenizer in [`native/html`](../native/html). The library does not link the tokenizer: a **Page** runs in a process of its own ([ADR-0009](adr/0009-a-process-for-each-page.md)). See [The HTML tokenizer's tests](#the-html-tokenizers-tests).

Run the test program, which needs a network connection:

```bash
out/mitto/mitto_core_test
```

On Windows it is `out\mitto\mitto_core_test.exe`.

It fetches `https://example.com/` unless given another URL, a second argument gives it the settings that `mitto_session_create()` reads, and a third names one of the scenarios at the end of its source, such as `cancel-after-response`. The first line of its output shows the status and the protocol:

```text
status 200 over h2
```

A first build took about 5 minutes on a 10-core Mac and on a 16-core Linux machine, and most of the time goes to `//net` and its dependencies. On macOS arm64 the library is 17 MB, 11 MB once stripped with `strip -x` and 5.4 MB compressed. On Linux x86_64 it is 103 MB, 12 MB once stripped and 6.0 MB compressed. [Size of the library](#size-of-the-library) says where that goes.

On Windows x64, for Chromium 157.0.8103.39, the first build took 9 minutes, 4,082 steps, on the same 16-thread machine that builds it in 5 minutes on Linux, with Windows' real-time virus scan on. The DLL is 13.4 MB and 6.5 MB compressed. It needs no stripping, because its debug information is in `mitto_core.dll.pdb` beside it, 92 MB, and the build directory takes 1.3 GiB.

On Linux the library needs only glibc and libgcc from the system at run time, as `ldd` shows, and a test checks this. On Windows it loads only Windows' own DLLs, and no C runtime of Visual Studio, because Chromium's build links the C runtime into it. A test reads that from the DLL's import tables. [`tools/workspace/args.gn`](../tools/workspace/args.gn) turns off GLib, GIO, D-Bus, NSS and NSPR, and a patch to `//net` makes that possible for NSS and NSPR. So on Linux the Chrome Root Store is the only source of trust: certificates added to an NSS database are ignored ([ADR-0006](adr/0006-linux-library-without-system-libraries.md)).

### The HTML tokenizer's tests

The HTML tokenizer follows the HTML Standard's tokenization section, and Blink's `HTMLTokenizer` where Blink emits something else, which the code says at each place. [`native/test/html5lib_tokenizer`](../native/test/html5lib_tokenizer) holds html5lib's tokenizer tests as the **Engine Release**'s tree has them, and [its `ORIGIN.md`](../native/test/html5lib_tokenizer/ORIGIN.md) says where from. They are in the repository because CI has no checkout, and the source export holds only what a build reads. Run them from `$MITTO_WORKSPACE/chromium-<tag>/src`, where `mitto` is the repository's `native/`:

```bash
out/mitto/html_tokenizer_test mitto/test/html5lib_tokenizer mitto/test/html5lib_tokenizer_failures.txt
```

It runs each test once for each of its initial states, with its whole input in one piece and again one code unit at a time, and the two must give the same tokens and parse errors. [`native/test/html5lib_tokenizer_failures.txt`](../native/test/html5lib_tokenizer_failures.txt) lists the tests the tokenizer fails, under the reason for each. The program fails if a test it lists passes or one it does not list fails, and prints what it expected and what it got. `--print-failures` prints every test that fails in the form of the list.

The native job of CI runs it on every Host, and the `asan` and `ubsan` builds of the [sanitizer workflow](#sanitizer-builds-and-the-fuzz-target) run it too.

## Build the Runtime

The **Runtime** runs a **Page**'s scripts in a process of its own, on the **Engine Release**'s V8 ([ADR-0008](adr/0008-v8-and-bindings-from-web-idl.md), [ADR-0009](adr/0009-a-process-for-each-page.md)). Its targets are in [`native/page`](../native/page) and [`native/bindings`](../native/bindings), and the group `//mitto:runtime` builds them. It exists only in a build directory that asks for it with the GN argument `mitto_enable_runtime`, which is off in [`tools/workspace/args.gn`](../tools/workspace/args.gn). [`tools/workspace/runtime_args.gn`](../tools/workspace/runtime_args.gn) turns it on, with the other arguments that the Runtime needs. The [source export](#build-from-the-source-export) carries what it reads, so it also [builds from there](#the-runtime-from-the-source-export), with no checkout. That build directory builds the Runtime and not the library, which `out/mitto` builds as before: [one of those arguments](#icus-data-and-v8s-snapshot) would clash with the library on Linux.

From `$MITTO_WORKSPACE/chromium-<tag>/src`, with depot_tools on your `PATH`, make a build directory of its own from both files and build it:

```bash
mkdir -p out/runtime
cat /path/to/mitto/tools/workspace/args.gn /path/to/mitto/tools/workspace/runtime_args.gn > out/runtime/args.gn
gn gen out/runtime --root-target=//mitto '--root-pattern=//mitto:*'
autoninja -C out/runtime runtime
```

On Windows, in PowerShell, with the checkout's ninja as for the native core:

```powershell
New-Item -ItemType Directory -Force out\runtime | Out-Null
Get-Content C:\path\to\mitto\tools\workspace\args.gn, C:\path\to\mitto\tools\workspace\runtime_args.gn | Set-Content out\runtime\args.gn
gn gen out\runtime --root-target=//mitto '--root-pattern=//mitto:*'
third_party\ninja\ninja.exe -C out\runtime runtime
```

A compile job of V8 needs far more memory than one of the library, and ninja runs as many jobs as the machine has cores, plus two. On a Mac with 16 GB, a build at that default took about 14 GiB with the system's compression of memory, and left the other programs on the machine with almost nothing, so build V8 there with about 5 jobs, such as `autoninja -j5 -C out/runtime runtime`, or on a larger machine.

This builds four things:

- `mitto_page`, the Page program, `out/runtime/mitto_page` and `mitto_page.exe` on Windows. The library starts it for each Page, with `--session=` and its end of the channel, and it then serves that Page's Session, as [runtime.md](runtime.md) describes. Given a script instead, it starts V8, runs the script in a new context and prints what the script returns, as `String()` writes it. A script that throws makes it print what was thrown to standard error and exit with 1. `--version` prints V8's version. Each context has a `Window` for its global object and the interfaces that the Runtime implements, such as `Document`, `Element` and `EventTarget`, with Chrome's members and messages, and a `document`: about:blank's, or with `--html=MARKUP` before the script, the one that the HTML parser builds from that markup ([runtime.md](runtime.md#the-document)). A script of `-` makes it read the script from standard input, for one longer than a command line holds: Windows' takes 32,767 characters. Scripts and markup are UTF-8, also on Windows, where the program reads its command line in UTF-16. `--enable-blink-features=` and `--disable-blink-features=`, before the script, turn runtime-enabled features on and off by name, as in Chrome ([runtime.md](runtime.md#runtime-enabled-features)).
- `html_tree_builder_test`, which runs html5lib's tree construction tests against the HTML parser's tree builder in [`native/dom`](../native/dom): see [The tree builder's tests](#the-tree-builders-tests).
- The Web IDL database, `out/runtime/gen/mitto/bindings/web_idl_database.pickle`: see [below](#the-web-idl-database).
- The bindings, which the bindings generator writes from the database into `out/runtime/gen/mitto/bindings`, with a listing of every interface and member it plans, `web_idl_listing.txt`, and the generator's own test, which checks that it refuses an extended attribute it does not know ([runtime.md](runtime.md#the-bindings)). The generator reads the whole database on every build, so a construct it does not handle stops the build with a message that names it.

```text
$ out/runtime/mitto_page --version
15.7.112.23
$ out/runtime/mitto_page 'typeof WebAssembly'
object
$ out/runtime/mitto_page --html='<title>A</title><p class=x>Hello' 'document.querySelector(".x").outerHTML'
<p class="x">Hello</p>
```

[`tests/test_page_program.py`](../tests/test_page_program.py), [`tests/test_page_bindings.py`](../tests/test_page_bindings.py) and [`tests/test_page_document.py`](../tests/test_page_document.py) run the program that `MITTO_PAGE_PROGRAM` names, and skip without it. CI runs them on every Host, against the program in the wheel that it installs ([In CI](#in-ci)), and on Linux against builds of the program with AddressSanitizer and UndefinedBehaviorSanitizer ([Sanitizer builds](#sanitizer-builds-and-the-fuzz-target)).

The build needs one thing from the checkout that the native core does not: the profiles by which Chrome's releases lay out V8's builtins, about 20 MB, which [`tools/workspace/bootstrap.py`](../tools/workspace/bootstrap.py) fetches with V8's own `v8/tools/builtins-pgo/download_profiles.py`, as a hook of Chromium's `DEPS` does when `checkout_pgo_profiles` is set. That setting would also fetch Chrome's own profiles for its compiler, which nothing here reads.

### The tree builder's tests

The HTML parser's tree builder follows the HTML Standard's tree construction for simple documents, and records each rule it skips for a document that needs more ([runtime.md](runtime.md#the-tree-builder)). [`native/test/html5lib_tree`](../native/test/html5lib_tree) holds html5lib's tree construction tests, from the same copy of html5lib as the tokenizer's, and [its `ORIGIN.md`](../native/test/html5lib_tree/ORIGIN.md) says where from. Run them from `$MITTO_WORKSPACE/chromium-<tag>/src`, where `mitto` is the repository's `native/`:

```bash
out/runtime/html_tree_builder_test mitto/test/html5lib_tree mitto/test/html5lib_tree_failures.txt
```

A test passes when it builds the tree it expects. One that builds another tree is fine only if the tree builder recorded a rule it skipped, and [`native/test/html5lib_tree_failures.txt`](../native/test/html5lib_tree_failures.txt) lists, under the reason for each, the tests that build another tree without skipping a rule. The program fails if a test it lists passes or skips a rule, or if one it does not list fails, and prints what it expected and what it got, and at the end how many tests skipped each rule. For Chromium 157.0.8103.39, 1,020 of the 1,615 tests that a Page can run pass, 594 skip a rule, and one is listed. `--print-failures` prints every test that fails in the form of the list, and `--print-skipped` each test that skipped a rule, with the rules. It runs the parser on V8's C++ heap, as the Page program does, and links the same data.

The native job of CI runs it on every Host, and the `runtime-asan` and `runtime-ubsan` builds of the [sanitizer workflow](#sanitizer-builds-and-the-fuzz-target) run it too.

### V8's settings

V8 builds with the settings that Chrome's releases build it with on 64-bit desktops ([ADR-0008](adr/0008-v8-and-bindings-from-web-idl.md)). Chrome's releases come from official builds, and Chromium's tree keeps the GN arguments of its official builder for each OS in `infra/config/generated/builders/ci/<os>-official/gn-args.json`. For Chromium 157 they are `is_official_build = true`, the OS and the CPU, and a few that only say how the build runs on Google's machines. Everything else that V8 builds with follows from those, by V8's own defaults in `v8/gni/v8.gni` and `v8/BUILD.gn`, so the Runtime's build takes the same defaults for the same CPU and OS. Where V8 decides by `is_official_build`, which the library's build leaves off, `runtime_args.gn` sets what an official build would.

| Setting | Value | Where it comes from |
| --- | --- | --- |
| Turbofan, `v8_enable_turbofan` | On | `v8/gni/v8.gni`: on unless in V8's lite mode, which only Chrome on iOS without Blink and on tvOS turns on |
| Maglev, `v8_enable_maglev` | On | `v8/BUILD.gn`: with Turbofan, on x64 and arm64 |
| Sparkplug, `v8_enable_sparkplug` | On | `v8/BUILD.gn`: unless V8 is built without compilers, `v8_jitless`, which follows lite mode |
| WebAssembly, `v8_enable_webassembly` | On | `v8/gni/v8.gni`: unless in lite mode |
| DrumBrake, V8's interpreter of WebAssembly, `v8_enable_drumbrake` | Off | `v8/gni/v8.gni`: only tvOS turns it on |
| Pointer compression, `v8_enable_pointer_compression`, in a cage that isolates share | On | `v8/gni/v8.gni`: on x64 and arm64. The cage follows it in `v8/BUILD.gn` |
| The external code space, `v8_enable_external_code_space` | On | `v8/BUILD.gn`: with pointer compression in a shared cage, on x64 and arm64 |
| V8's sandbox, `v8_enable_sandbox` | On | `v8/BUILD.gn`: with pointer compression in a shared cage and the external code space |
| cppgc's caged heap, `cppgc_enable_caged_heap` | On | `v8/BUILD.gn`: on x64 and arm64 |
| `Intl`, `v8_enable_i18n_support` | On | `v8/gni/v8.gni`. The program gives it ICU's whole data file, [as Chrome does](#icus-data-and-v8s-snapshot) |
| Temporal, `v8_enable_temporal_support` | On | `v8/gni/v8.gni`: everywhere but in Node.js |
| The startup snapshot in a file of its own, `v8_use_external_startup_data` | On | `v8/gni/v8.gni`: everywhere but iOS. The program holds that file |
| Builtins laid out by profiles, `v8_enable_builtins_optimization` | On | `v8/gni/v8.gni`: with Chrome's PGO, which official builds use on Windows, macOS and Linux (`build/config/compiler/pgo/pgo.gni`). Set in `runtime_args.gn` |
| V8's logging, `v8_logging_level` | 0: a failed check ends the process without a message | `v8/BUILD.gn`: 0 in official builds. Set in `runtime_args.gn` |
| V8's helpers for fuzzers, `v8_wasm_random_fuzzers` | Off | `v8/gni/v8.gni`: off in official builds. Set in `runtime_args.gn` |
| Instance types and layouts of V8's objects from metagen, `v8_use_metagen` | Off, from Torque | On in Chrome, as `v8/gni/v8.gni` has it. Set in `runtime_args.gn`: see below |

[`tools/workspace/v8_settings.py`](../tools/workspace/v8_settings.py) checks this on the Host it runs on, in the workspace's checkout, and [the release routine](releasing.md#2-check-out-the-new-engine-release) runs it on each:

```bash
export MITTO_WORKSPACE=/absolute/path/to/mitto-workspace
uv run python -m tools.workspace.v8_settings
```

It asks gn what V8 compiles with under `args.gn` followed by `runtime_args.gn`, and under the arguments of the Host's official builder: the defines of V8's code, of its builtins, of cppgc and of V8's platform, and the arguments of `mksnapshot`, which writes the builtins. The official builder's arguments go in without those that only say how the build runs and how much debug information it keeps, and with `chrome_pgo_phase = 0`, because Chrome's own profiles for its compiler are not in the checkout and only change how the compiler lays out code that it has anyway. Without them `v8_enable_builtins_optimization` would follow PGO off, so it stays on there as in Chrome. The script prints every difference, and exits with 1 if there is one. It accepts three, which `DIFFERENT_ON_PURPOSE` in the script explains: `OFFICIAL_BUILD`, which an official build defines for all of Chromium and which in V8 only decides whether its compiler keeps the names of its operators for debugging; `USE_GLIB=1`, which Chrome's Linux build defines for all of Chromium because it links GLib, which `args.gn` leaves out of the library and so of the Page program, and which no code of V8 reads; and `V8_USE_METAGEN=0`, below. For Chromium 157.0.8103.39 it found no other difference on any Host. Without the three settings of `runtime_args.gn`, it reports each of them, `V8_ENABLE_BUILTINS_OPTIMIZATION`, `V8_LOGGING_LEVEL` and `V8_WASM_RANDOM_FUZZERS`, and the profile that `mksnapshot` reads.

The one setting that differs from Chrome's is where V8's build takes the instance types and the layouts of V8's objects from. Chrome's build reads them from V8's headers with metagen, which loads `third_party/llvm-libclang`. Metagen asks gn for the flags it reads those headers with, by `gn desc` over all of Chromium's build graph, without the two switches above, and under `args.gn` that graph does not generate on Linux: `ui/gtk` asserts that GIO is on, and `args.gn` turns it off for the library. The source export also holds none of the build files that such a `gn desc` reads, on any Host. So `runtime_args.gn` takes them from V8's Torque compiler, as V8's build did before metagen, and as it still can while it moves to metagen (`v8/src/objects/instance-types-gen.h`). For Chromium 157.0.8103.39 on macOS, a Page program built each way was the same in every section, and V8's snapshot and builtins (`snapshot_blob.bin` and `gen/v8/embedded.S`) were the same byte for byte.

The script compares with the official builder of the Host's own OS, because the Page program is built for that OS as Chrome is. The three official builders differ only in their OS and CPU, and in arguments that only say how their builds run.

What differs from Chrome's renderers is outside V8's build. A renderer gets V8's flags from Chrome's features at run time, starts from Blink's own snapshot of a context with its bindings, and allocates its memory with PartitionAlloc, which the library leaves out (`use_allocator_shim = false` in `args.gn`) and the Page program inherits. V8 itself still puts its `ArrayBuffer`s into PartitionAlloc, as in Chrome (`v8_enable_partition_alloc`, which follows the shared cage). The milestones that compare what a page sees with Chrome decide each of these.

### ICU's data and V8's snapshot

The Page program holds ICU's whole data file, `third_party/icu/common/icudtl.dat`, 10.8 MB, and V8's startup snapshot, `snapshot_blob.bin`, which V8's build writes next to it. [`native/page/embed_data.py`](../native/page/embed_data.py) writes each into an assembly file while the program builds, as [`native/make_icu_data.py`](../native/make_icu_data.py) does for the library. When it starts, the program gives ICU its data with `udata_setCommonData()` and V8 its snapshot with `v8::V8::SetSnapshotDataBlob()`, as Chrome's renderers do with the files that the browser maps for them, so ICU and V8 read the same bytes as in Chrome.

They go inside the program rather than beside it as files for two reasons. The Page process opens no files of its own, so that Chromium's sandbox can hold it before the Runtime ships ([ADR-0009](adr/0009-a-process-for-each-page.md)), and the program then needs neither a file nor a handle from the library to start. And the wheel then ships one more file, not three. The data costs the same either way: the wheel compresses it, and the system maps it into memory from the program's file as it would from a data file, once for all Pages.

The library still holds only the few items of ICU's data that its own code reads ([ICU's data](#icus-data)). The Page program links no part of the library, and the library links no part of V8.

`runtime_args.gn` also gives ICU's build its stub data and the copy of the data file in the build directory, which `args.gn` turns off for the library. V8's `mksnapshot` runs during the build, links ICU and reads ICU's data from the file next to it, as in Chromium's own builds, and the stub gives it the symbol that ICU looks up its data by. The library defines that symbol itself (`//mitto:icu_data`). On macOS and Windows the linker then leaves the stub out of the library, but on Linux Chromium's build links a shared library with `--whole-archive`, so every object of ICU's static library goes into it, the stub's too, and the two clash. That is why a Runtime build directory does not build the library.

### The Page program stands alone

`mitto_page` links V8, ICU's code and the parts of `//base` it uses, and not the library. The library exports only its C API, and V8's API is C++, so the program could only reach V8 through the library if the library exported it, with its own copy of libc++ and everything else that Chromium builds. The library would also grow by all of V8 for every caller who never opens a Page. The cost is that the two each hold their own copy of the code they share, such as `//base` and ICU's code.

The library builds as before: its target, its sources and `args.gn` are unchanged, and nothing of the Runtime is in its build graph while `mitto_enable_runtime` is off. On macOS, where the linker leaves the stub out, a library built in a Runtime build directory was also compared with one from `args.gn` alone, section by section as [a build from the export](#make-the-source-export) compares its library with its slice's. They were the same, with the same 20 exports and the same size.

### The Web IDL database

[`native/bindings/BUILD.gn`](../native/bindings/BUILD.gn) builds Blink's Web IDL database, as Blink's `web_idl_database` target in `third_party/blink/renderer/bindings/BUILD.gn` does: it runs Blink's `collect_idl_files.py` on Blink's lists of IDL files in `idl_in_core.gni` and `idl_in_modules.gni`, and then `build_web_idl_database.py` on the result, with `runtime_enabled_features.json5`. Blink's own targets cannot be used as they are, because their visibility leaves out everything outside Blink. They also read the IDL files that only Blink's tests see, such as those of `internals`, which a page never has and which Blink generates from files of its own, so this database leaves them out. That includes `core/timing/internals_profiler.idl`, which Blink lists with the files of pages but which only adds to `Internals`, so that the database cannot be built without Blink's tests. For Chromium 157.0.8103.39 it reads 2,177 of the 2,237 IDL files under `third_party/blink/renderer`: 864 of Blink's core and 1,313 of its modules. Of the others, 54 are for Blink's tests, 4 for Android's WebView, 1 for ChromeOS alone, and 1 is in no list of Blink's build. The database file is 12.9 MB. The [bindings generator](runtime.md#the-bindings) reads it. Its build reads each of these IDL files and Blink's scripts with the modules they import, so a source export made from a build of the Runtime carries them all.

### Times and sizes

Measured for Chromium 157.0.8103.39, each from an empty build directory:

| | Linux x86_64 | macOS arm64 | Windows x64 |
| --- | --- | --- | --- |
| Machine | 16 threads | 10 cores | 16 threads |
| Cold build of `runtime` in `out/runtime` | 15 minutes | 25 minutes | 22 minutes |
| Steps | 4,441 | 4,473 | 4,415 |
| `mitto_page` as built | 236.27 MB | 60.22 MB | 44.70 MB |
| `mitto_page`, stripped as the wheel strips the library | 42.21 MB | 40.73 MB | 44.70 MB |
| `mitto_page`, stripped and compressed with deflate | 16.08 MB | 15.22 MB | 16.98 MB |
| Build directory | 2.3 GiB | 1.5 GiB | 2.4 GiB |

Of the stripped program, ICU's data is 10.8 MB and V8's snapshot 0.37 MB. A wheel that carried the program would grow by about its compressed size, nearly three times what the wheel is today. On Linux the program needs only glibc and libgcc from the system, as `ldd` shows, like the library. On Windows, as for the library, the program holds no debug information to strip: it is in `mitto_page.exe.pdb` beside it, 301 MB. Sizes are in MB of 1,000,000 bytes.

## Build from the source export

A hosted runner has no room for a Chromium checkout, so the native core has a second way to build: from the **source export**, an archive of the files of Chromium that its build reads on each **Host**, with the patches already applied. It needs no workspace and no depot_tools.

Run this from the repository root, with a new directory on a path without spaces. It needs about 4.2 GiB there on Linux, 3.2 GiB on macOS and 4.7 GiB on Windows, including the build output, as well as `curl` and `tar`, which Windows brings: there the command takes them from `System32`, whatever else is on `PATH`. It took about 6 minutes on a 10-core Mac, one of them for the downloads, and about 10 minutes on a 16-thread Windows machine. macOS also needs the same Xcode as a checkout, and Windows the same Visual Studio and Windows SDK (see [Requirements](#requirements)). On Windows the longest path lies 198 characters below the directory, in the Runtime's build output, so a directory of up to 61 characters keeps every path within Windows' limit of 260.

```bash
uv run tools/workspace/export.py build /absolute/path/to/new/directory
```

On Windows, in PowerShell, with a short path such as `C:\ex`:

```powershell
uv run tools/workspace/export.py build C:\ex
```

The command:

1. downloads the archive that [`tools/workspace/source_export.json`](../tools/workspace/source_export.json) names and checks its hash, or uses a local archive given with `--archive`;
2. unpacks it as `chromium-<tag>`, with `src` next to an `export.json` that lists the toolchain;
3. on Windows, runs the export's `build/vs_toolchain.py`, Chromium's own script that picks the Visual Studio a build uses, and stops unless it finds one and the Windows SDK has the version that the script asks for. The script knows the SDK only by that version, so a missing SDK would otherwise stop the build later, with a message that does not say so;
4. downloads this Host's toolchain from Chromium's own storage and checks each package's hash;
5. links the repository's `native/` in as `//mitto`, a junction on Windows, and copies [`tools/workspace/args.gn`](../tools/workspace/args.gn), so that these two come from the repository and not from the export;
6. runs `gn gen --check`, so that an include that `gn check` refuses stops the build before it compiles anything;
7. runs `ninja`, says whether the library is the same as the one the export's slice built, as it is until `native/` changes, and prints the library's path.

Running it again continues where it stopped, and rebuilds what changed in `native/`. It refuses a directory that holds another export of the same release, so a new export needs a new directory. To run the tests against the library:

```bash
export MITTO_NATIVE_LIBRARY=/absolute/path/to/new/directory/chromium-<tag>/src/out/mitto/libmitto_core.so
uv run pytest
```

On macOS the file is `libmitto_core.dylib`. On Windows, in PowerShell:

```powershell
$env:MITTO_NATIVE_LIBRARY = 'C:\ex\chromium-<tag>\src\out\mitto\mitto_core.dll'
uv run pytest
```

### The Runtime from the source export

The same command builds [the Runtime](#build-the-runtime), the Page program and the Web IDL database, in `out/runtime`, from `args.gn` followed by `runtime_args.gn`, as in a checkout:

```bash
uv run tools/workspace/export.py build /absolute/path/to/new/directory --config runtime -j 5
```

`-j` sets how many build steps ninja runs at once, which by default is the number of cores plus two. V8's compile jobs need far more memory than the library's, so on a Mac with 16 GB build it with about 5. The command says whether `mitto_page` is the same as the one that the export's slice built, in the same way as for the library, and prints its path. The database is `out/runtime/gen/mitto/bindings/web_idl_database.pickle` beside it. The build of the library and the Runtime's can share one directory, each in its own build directory under `out/`, and then share the sources and the toolchain too.

Measured for Chromium 157.0.8103.39, in a directory where the library's build had run first:

| | Linux x86_64 | macOS arm64 | Windows x64 |
| --- | --- | --- | --- |
| Machine | 16 threads | 10 cores, `-j 5` | 16 threads |
| Build of `runtime` | 18.5 minutes | 20 minutes | 22 minutes |
| `out/runtime` | 2.3 GiB | 1.6 GiB | 2.4 GiB |

[CI](#in-ci) builds it the same way on every Host.

### Sizes

Measured for Chromium 157.0.8103.39, `mitto-chromium-157.0.8103.39-3.tar.xz`. The archive is the same for every Host: 34.3 MiB compressed, 278 MiB unpacked, 20,063 files. Of those, 17,619 are in every slice. The Windows slice alone adds 815, such as `base/win`, nasm, which BoringSSL's assembly needs there, and the Rust crates of Windows' API, the Linux slice alone 262 and the macOS slice alone 289.

| Slice | Files | Unpacked |
| --- | --- | --- |
| Linux x86_64, from five builds | 18,957 | 232 MiB |
| macOS arm64, from two builds | 18,930 | 231 MiB |
| Windows x64, from two builds | 18,492 | 256 MiB |

[The Runtime](#build-the-runtime)'s build reads 7,084 files that the native core's builds do not, 79 MiB unpacked, which make the archive 9.2 MiB larger than the 25.1 MiB of the export before it: V8, 3,122 files and 65 MiB, of which its two builtins profiles, `x64.profile` and `x64-rl.profile`, are 10.7 MB; Blink's IDL files and the scripts that read them, 2,229, with the Python projects that those scripts import, `tools/idl_parser`, ply, pyjson5, jinja2 and markupsafe; llvm-libc, 1,067; the Rust crates of V8's Temporal and libm, 547; and highway, fast_float, fp16 and dragonbox. The toolchain of Linux and macOS was measured for Chromium 157.0.8103.27, and is the same for 157.0.8103.39.

| Toolchain package | Comes from | Linux x86_64: download | unpacked | macOS arm64: download | unpacked | Windows x64: download | unpacked |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Clang | Google Storage | 55 MiB | 325 MiB | 47 MiB | 303 MiB | 49 MiB | 224 MiB |
| Rust | Google Storage | 274 MiB | 1.41 GiB | 246 MiB | 1.28 GiB | 406 MiB | 2.41 GiB |
| Linux sysroot | Google Storage | 19 MiB | 221 MiB | | | | |
| `rc.exe`, the resource compiler | Google Storage | | | | | under 1 MiB | under 1 MiB |
| CPython, for build scripts | CIPD | 46 MiB | 152 MiB | 24 MiB | 82 MiB | 19 MiB | 51 MiB |
| mold, the Linux linker | CIPD | 12 MiB | 41 MiB | | | | |
| gn | CIPD | 4 MiB | 9 MiB | 1 MiB | 2 MiB | 1 MiB | 3 MiB |
| ninja | CIPD | under 1 MiB | under 1 MiB | under 1 MiB | under 1 MiB | under 1 MiB | under 1 MiB |
| Total | | 410 MiB | 2.14 GiB | 319 MiB | 1.66 GiB | 476 MiB | 2.68 GiB |

On Windows, Visual Studio and the Windows SDK come from the machine, as Xcode does on macOS: Chromium's own bots take them from a package that only Google's machines can download. The native core never runs `rc.exe` today, but gn's toolchain for Windows names it, and a resource file in `native/` would need it.

### Why the toolchain is not an asset

These packages come from Google Storage and CIPD, Chromium's package store, rather than from a release asset of ours. `DEPS` pins each of them for a Chromium release, they are the very packages Chromium's bots build with, and they stay available for old releases. An asset for each Host would fit within GitHub's 2 GiB limit, but it would only copy 300 to 500 MiB per Host and per **Engine Release** for no gain in what is checked: `export.json` records each package's SHA-256, and the build refuses anything else. If a package ever disappears from Chromium's storage, an asset can take over by changing its URL in `export.json`.

### In CI

A job per Host in [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) runs this command, runs it again with `--config runtime` in the same directory for [the Runtime](#the-runtime-from-the-source-export), [builds a wheel](#build-a-wheel) from the library and the Page program and runs `pytest` against the installed wheel. Builds on Linux and macOS are from 14 runs from a cold cache for Chromium 157.0.8103.27, and on Windows from two for 157.0.8103.39, and disk use is from the first. The range is mostly the runners: the same `native/` took 21 and 27 minutes on macOS. On Linux and macOS, the tests and the whole jobs are from the runs of 2026-11-08 and 2026-11-09, when the suite had about 6,800 tests. On Windows, the tests are from four runs, and each whole job from two.

| | Linux x86_64 | macOS arm64 | Windows x64 |
| --- | --- | --- | --- |
| Runner | `ubuntu-latest`, 4 cores, 16 GB of memory | `xcode-27`, 3 cores, 7 GB of memory | `windows-2025-vs2026`, 4 cores, 16 GB of memory |
| Install of the Windows SDK | | | 1.7 to 2.8 minutes, in a job that builds |
| Build | 13 to 24 minutes, under one of them for the downloads | 14 to 28 minutes, under one of them for the downloads | 40 and 41 minutes, under one of them for the downloads |
| Tests | 7.5 to 9 minutes | 8 to 9.5 minutes | 10.5 minutes |
| Peak disk use of the job | 4.3 GiB, of which 4.1 GiB is the build's directory | 3.5 GiB, of which 3.2 GiB is the build's directory | 5.0 GiB on `D:`, of which 4.7 GiB is the build's directory, and 1.2 GiB on `C:` for the SDK |
| Disk free when the job starts | 86 GiB | 37 GiB | 147 GiB on `D:`, where the job works, and 31 GiB on `C:` |
| Whole job | 23 to 34 minutes | 25 to 37 minutes | 55 minutes, both times |
| Whole job with a cached library | 8 to 9.5 minutes | 8.5 to 10 minutes | 11 minutes |

The table above is from before the job built the Runtime. With it, on 2026-11-11, in one run from nothing and in runs that took the builds from the caches, on a branch whose pushes ran the job:

| | Linux x86_64 | macOS arm64 | Windows x64 |
| --- | --- | --- | --- |
| Build of the library | 21 minutes | 18 minutes | 24 minutes |
| Build of the Runtime, after it | 74 minutes | 83 minutes, with `-j 3` | 58 minutes |
| Build of the wheel with the Page program | 12 seconds | 11 seconds, or 50 when `dsymutil` reads the object files of a build that the job made | 20 seconds |
| Tests | 9 minutes | 9 to 9.5 minutes | 9.5 to 11 minutes |
| Whole job from nothing | 104 minutes | 112 minutes | 94 minutes |
| Whole job after a change in `native/page`, from `main`'s build directories | 11 minutes, of which the Runtime's build took 4 seconds | 13 minutes, the Runtime 8 seconds | 16 minutes, the Runtime 13 seconds |
| Whole job with both builds cached | 9.5 minutes | 10.5 minutes | 12.5 minutes |
| `out/runtime` | 2.3 GiB | 1.6 GiB | 2.5 GiB |
| Peak disk use of the job | 7.4 GiB | 5.7 GiB | 7.9 GiB on `D:` |
| Disk free when the job starts | 88 GiB | 36 GiB | 220 GiB on `D:` |

So a job that builds from nothing, as `main` does after each merge that changes `native/` or `tools/workspace/`, and as a pull request does that changes `tools/workspace/`, takes 94 to 112 minutes. The job's timeout is 180 minutes, and GitHub stops a job on a hosted runner after 6 hours. On Linux the job used at most 6.9 GiB of the runner's 16 GB of memory. On macOS the Runtime's build runs 3 steps at once, as many as the runner has cores: at ninja's default of 5, the build of the Runtime alone took 108 minutes on another runner of the same image, where the runner's 7 GB of memory never ran short enough to swap.

On 2026-10-30 the Windows image had only the Windows SDK 10.0.26100, and Chromium 157's `build/vs_toolchain.py` asks for `10.0.28000.0`. So a Windows job that builds, and only such a job, first installs the SDK 10.0.28000.2270, unless the image already has that version. The installer comes from Microsoft's downloads page and is checked against its SHA-256. It installs only the feature for desktop C++ apps on x64, `OptionId.DesktopCPPx64`, which brings in 46 packages: the headers and libraries for x64 and x86 and the SDK's own tools, 1.2 GiB. The build needs nothing more. That took 1.7 to 2.8 minutes in three runs, downloads included, so the job installs the SDK each time rather than keeping its `10.0.28000.0` directories in the Actions cache, where they would take 1.2 GiB of the room that this repository's caches share, to save those two minutes at most. The version and the installer are pinned in [`.github/actions/install-windows-sdk/action.yml`](../.github/actions/install-windows-sdk/action.yml), which the [release workflow](#the-release-workflow) uses too, and change only when a new **Engine Release** asks for another SDK.

On Windows 11 22H2 and later, and on Windows Server 2025, the Windows of `windows-2025-vs2026`, `//net` asks Windows for a random source port before each TCP connect, with `SO_RANDOMIZE_PORT`, as Chrome there does (`kTcpPortRandomizationWin`). It does the same before it connects the UDP socket of each QUIC connection, on any Windows. Windows refuses about one such connect in 7,000 with `WSAENOBUFS`, however few ports are in use, or with `WSAEADDRINUSE` when the port it drew is still in TIME_WAIT towards the same server, which a request would report as `ERR_NO_BUFFER_SPACE` or `ERR_ADDRESS_IN_USE`. It refuses before it sends anything. So on Windows every Session makes its TCP and UDP sockets through [`native/connect_retrying_socket_factory.cc`](../native/connect_retrying_socket_factory.cc), which tries a connect that Windows refused so again, up to two more times, each with a new socket and so a new random port. A server sees one ordinary connect or QUIC handshake, and a request fails only where Windows refuses all three. Chrome there fails the request at once, which is the one way a Session differs from it here. The setting `refused_connects` of [`mitto.h`](../native/include/mitto/mitto.h), for tests, refuses connect attempts in the same way on every Host, so the tests of the retry run on each.

On `windows-2025-vs2026`, on 2026-11-09 and before the retry, 3 of 18 runs of the whole suite from the installed wheel failed, each with one or two requests that got `ERR_NO_BUFFER_SPACE`, and so did 2 of 18 runs with the range widened as below, one of them over QUIC. During those runs at most about 2,770 of the 16,384 dynamic ports were in use, and at each failure 13,800 or more were free, so the ports did not run out. TIME_WAIT lasts 120 seconds there, and Docker's services exclude 200 ports of the range, which made no difference once they stopped. A plain Python client saw about one refusal in 7,000 connects with the option, in each fifth of the range from empty to 90% full, about one in 138,000 with the range widened to 45,536 ports, and none in about 150,000 connects and binds without the option. With the retry, on 2026-11-10, 10 runs in a row of the release workflow's steps on `windows-2025-vs2026` passed, each with the Windows SDK installed, the native core built from nothing and every test run from the installed wheel.

The Windows job runs the same steps as the other Hosts, in Git's bash. Git for Windows sets `core.autocrlf` to true, which would check text files out with CRLF line ends, so the Windows jobs set it to false before the checkout, and every file has the bytes that the repository holds, as on the other Hosts.

The job keeps only the library, in the Actions cache: 21 MB on Linux, 6 MB on macOS and 28 MB on Windows, compressed. On Windows that includes the DLL's PDB, which the wheel's symbols archive holds. The key is the runner's name and a hash of `native/` and `tools/workspace/`, which holds the pin of the export. When none of that has changed, the job skips the build. A pull request can read the caches of `main` and its own, so one that changes neither directory uses the library that `main` built. One that changes either builds once, and `main` builds again after the merge.

A second cache keeps the build's directory, about 300 MB per **Host** and build, because what the export compiles to changes only with `tools/workspace/`. Its key is the runner's or the build's name and a hash of that directory. `main` saves it the first time it builds with that hash, and a pull request restores it and rebuilds only `native/`. In one trial, a change to one file in `native/` rebuilt 11 of about 4,000 objects on Linux and macOS. On Windows it rebuilt 27 of 4,082 steps, every object of `//mitto` and its links, in a minute and a half, and the whole job took 14 minutes. `main` never restores it, so each merge still builds from nothing and shows a mistake in an incremental build. A pull request that changes `tools/workspace/` also builds from nothing. CIPD packages are unpacked with a fixed file time, because ninja would otherwise rebuild what depends on them.

The Runtime is cached in the same two ways. The job keeps the Page program, with its PDB on Windows, which the wheel's symbols archive holds: 40 MB on Linux, 16 MB on macOS and 70 MB on Windows, compressed. Its key is `runtime-`, the runner's name and the same hash of `native/` and `tools/workspace/` as the library's, so it changes whenever anything that the Runtime builds from in this repository changes, as well as whenever the library's key does. `out/runtime` has a cache of its own, keyed like the library's build directory by a hash of `tools/workspace/` alone: 447 MB on Linux, 390 MB on macOS and 543 MB on Windows. `main` saves it the first time it builds with that hash, and a pull request restores it. After a change in `native/page`, the Runtime then built in 6 steps, the compile of `mitto_page.cc` and the link among them.

With the Runtime and [its sanitizer builds](#sanitizer-builds-and-the-fuzz-target), one set of `main`'s caches, for one hash of `native/` and `tools/workspace/`, takes about 5.8 GB of the 10 GB that GitHub keeps for a repository, where it took about 2.9 GB before: the Runtime adds 1.38 GB for its build directories, 1.2 GB for those of its two sanitizer builds and 0.29 GB for the programs. GitHub evicts the caches used least recently once a repository holds more than 10 GB, and every pull request that builds restores `main`'s build directories, so the caches that go first are those of older pull requests and those of a `tools/workspace/` that has changed since.

No cache keeps the export itself, so every job that builds downloads it from its release asset. Since it carries the Runtime it is 34.3 MiB rather than 25.1 MiB, so each such job downloads 9.2 MiB more, beside the toolchain's 319 to 476 MiB, and unpacks 79 MiB more into the build's directory. The library's caches do not grow from it: the library and `out/mitto` are what they were, because the native core's build reads the same files. A new pin changes `tools/workspace/`, though, so as with any new export each cache gets a new key, and the first build on each runner, on `main` and in the pull request that pins it, starts from nothing.

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
| `fuzz` | AddressSanitizer and libFuzzer | `session_settings_fuzzer`, `request_options_fuzzer`, `cookies_fuzzer`, `socks5_auth_fuzzer`, `client_hints_fuzzer`, `html_tokenizer_fuzzer`, `page_messages_fuzzer` | 2.2 GiB |
| `runtime-asan` | AddressSanitizer and LeakSanitizer | The Page program, the tree builder's test and the Web IDL database, as `runtime` does, instead of the library | 3.1 GiB |
| `runtime-ubsan` | UndefinedBehaviorSanitizer | The same | 2.7 GiB |

Each of them stops at the first report.

The fuzz targets have a build of their own because Chromium's fuzzing builds define `FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION`, which changes what BoringSSL does, so the library in that build is not the one that ships.

`runtime-asan` and `runtime-ubsan` add the sanitizer's arguments to [the Runtime](#build-the-runtime)'s, `args.gn` followed by `runtime_args.gn`, so each builds `//mitto:runtime` without the library in a directory of its own, as `runtime` does. Under UndefinedBehaviorSanitizer V8 turns on every check of `-fsanitize=undefined` for its own code, where Chromium picks a few (`v8/BUILD.gn`). The source export's Linux slice traces both, but `mitto-chromium-157.0.8103.39-3.tar.xz` was made before they existed and builds them all the same. To run the Page program's tests against one of them:

```bash
export MITTO_PAGE_PROGRAM=/absolute/path/to/new/directory/chromium-<tag>/src/out/runtime-asan/mitto_page
ASAN_OPTIONS=detect_leaks=1 uv run pytest tests/test_page_program.py tests/test_page_bindings.py tests/test_page_document.py
```

To run the C test program against a local [capture server](capture.md), with `mitto_core_test` from the `asan` or the `ubsan` build:

```bash
export MITTO_CORE_TEST=/absolute/path/to/new/directory/chromium-<tag>/src/out/asan/mitto_core_test
ASAN_OPTIONS=detect_leaks=1 uv run pytest tests/test_core_test_program.py
```

Over each of HTTP/1.1, HTTP/2 and HTTP/3 it makes a request, cancels and releases one before and after its response, destroys a Session and exits the process with one running. It also makes a request that fails on the certificate, tries settings that are refused, fetches a missing intermediate, and destroys a Session and exits the process during such a fetch. A report fails the test, and the test shows it. Chromium's default options leave leak checks off, so the command turns them on. Python cannot load a sanitized library, because the sanitizers' runtime is linked into the program that starts.

To fuzz the code that reads a Session's settings, from the repository root, with new inputs going into a directory of your own. `request_options_fuzzer` does the same for a request's options, `cookies_fuzzer` for cookies, `socks5_auth_fuzzer` for what a SOCKS5 proxy answers, `client_hints_fuzzer` for what a server asks for, `html_tokenizer_fuzzer` for HTML and `page_messages_fuzzer` for what a Page sends its Session, with the files of that name:

```bash
mkdir corpus
ASAN_OPTIONS=detect_leaks=1 /absolute/path/to/new/directory/chromium-<tag>/src/out/fuzz/session_settings_fuzzer -max_total_time=60 -dict=native/test/session_settings_fuzzer.dict corpus native/test/session_settings_corpus
```

[`.github/workflows/sanitizers.yml`](../.github/workflows/sanitizers.yml) runs one job per build on `ubuntu-latest`. It runs when a pull request or `main` changes `native/` or `tools/workspace/`, and every day, when the unchanged builds come from the cache and only the fuzzing takes real time. It fuzzes each target for five minutes, or for one minute on a pull request, and keeps any input that crashed one as an artifact of the run. Builds reuse objects from `main` in the same way as in the job above. These jobs are not required checks, because they do not run on every pull request.

Builds of the native core are from two runs from a cold cache, and those of the Runtime from one, on 2026-11-11, when the sizes were measured too. After a change in `native/page`, with `main`'s build directories, each of the Runtime's builds took under a minute, and their whole jobs about a minute.

| | `asan` | `ubsan` | `fuzz` | `runtime-asan` | `runtime-ubsan` |
| --- | --- | --- | --- | --- | --- |
| Build | 19 and 26 minutes | 17 and 20 minutes | 24 and 26 minutes | 63 minutes | 66 minutes |
| Cached, compressed | 65 MiB | 41 MiB | 182 MiB | 89 MiB | 70 MiB |
| Build directory in the cache | 426 MiB | 356 MiB | 622 MiB | 609 MiB | 536 MiB |

## Build a wheel

A wheel of mitto is for one **Host**. Its tag is `py3-none-PLATFORM`, and it holds the native library next to [`src/mitto/_native.py`](../src/mitto/_native.py), where the loader looks for it ([ADR-0003](adr/0003-c-abi-and-ctypes.md)). From the repository root, name a library built [from the source export](#build-from-the-source-export) or [in a checkout](#build-the-native-core):

```bash
MITTO_WHEEL_LIBRARY=/absolute/path/to/libmitto_core.dylib uv build --wheel
```

On Linux the file is `libmitto_core.so`. On Windows, in PowerShell:

```powershell
$env:MITTO_WHEEL_LIBRARY = 'C:\ex\chromium-<tag>\src\out\mitto\mitto_core.dll'
uv build --wheel
```

The build hook in [`tools/wheel.py`](../tools/wheel.py):

1. strips a copy of the library. On macOS it runs `dsymutil` and then `strip -x`, and checks that the copy's code signature still holds, because macOS on arm64 does not load a library whose signature does not. On Linux it runs `objcopy --only-keep-debug`, `strip` and `objcopy --add-gnu-debuglink`. A DLL holds no debug information to strip: its CodeView debug record names its PDB, `mitto_core.dll.pdb`, by file name alone, because Chromium's build links with `/pdbaltpath:%_PDB%`. The hook copies the DLL as it is, and takes the PDB of that name next to the library, once it has checked that the PDB's GUID and age are those the record names;
2. takes the wheel's platform from the stripped library: `macosx_X_0_arm64` from the `minos` of its `LC_BUILD_VERSION` on macOS, `manylinux_2_X_x86_64` from the highest glibc symbol version it needs on Linux, and `win_amd64` from the machine in its PE header on Windows. `auditwheel`, which checks manylinux wheels, has a policy for some versions of glibc only, and a version without one becomes the next version with one;
3. puts it into the wheel as `mitto/libmitto_core.dylib`, `mitto/libmitto_core.so` or `mitto/mitto_core.dll`;
4. writes what stripping removed into an archive next to the wheel, named after it: `mitto-<version>-py3-none-<platform>.symbols.tar.xz`. It holds the dSYM bundle on macOS, `libmitto_core.so.debug` on Linux and `mitto_core.dll.pdb` on Windows.

With `MITTO_WHEEL_PAGE_PROGRAM` naming [the Page program](#build-the-runtime) too, the hook also puts it into the wheel next to the library, as `mitto/mitto_page` or `mitto/mitto_page.exe`, stripped as the library is, and adds what stripping removed to the same archive: `mitto_page.dSYM`, `mitto_page.debug` or `mitto_page.exe.pdb`. CI's native job does that for its tests ([In CI](#in-ci-1)). Without the variable the wheel holds no Page program, and the [release workflow](#the-release-workflow) sets none, so a release's wheels stay as they were until the release that ships the Runtime ([roadmap](roadmap.md), M10). [`tests/test_wheel.py`](../tests/test_wheel.py) builds a wheel as the release workflow does, from a library with a Page program next to it, and checks that the wheel holds no Page program, and that the release workflow never names the variable. Run against an installed wheel, it also checks that the wheel holds a Page program only where `MITTO_PAGE_PROGRAM` names the one installed from it, as in CI.

Every wheel also holds `LICENSE` and the [third-party notices](#third-party-notices) of its library, [`THIRD_PARTY_NOTICES`](../THIRD_PARTY_NOTICES), in its `.dist-info/licenses` directory. `license-files` in `pyproject.toml` names both, and the wheel's `METADATA` lists them in `License-File` lines.

Without `MITTO_WHEEL_LIBRARY` the build stops, so that no wheel without a library can go out: it would install anywhere and fail on its first request. An editable install, which `uv sync` makes, needs no library, because the loader takes the one in `MITTO_NATIVE_LIBRARY`. mitto ships no sdist, because a build from source needs the source export and Chromium's toolchain.

The hook needs Xcode on macOS, `objcopy` and `strip` from GNU binutils on Linux, and nothing more on Windows. On macOS the library only points to the debug information, which stays in the build's object files. For a library whose build directory is gone, such as one restored from CI's cache, `dsymutil` warns about each missing file, and the symbols keep function names but lose line numbers.

To turn an address in the library, such as one from a crash report, back into a function name, unpack the symbols archive and run one of these, on macOS and on Linux, with the address's offset from the start of the library in place of `0x3c44`:

```bash
atos -o libmitto_core.dylib.dSYM -l 0 0x3c44
addr2line -f -C -e libmitto_core.so.debug 0x3c44
```

On Windows, put `mitto_core.dll` from the wheel next to `mitto_core.dll.pdb` from the archive, and run LLVM's `llvm-symbolizer`, such as the one in the toolchain of a [build from the source export](#build-from-the-source-export), under `third_party\llvm-build\Release+Asserts\bin`. A crash report names the address as `mitto_core.dll+0x3c44`, an offset from the start of the DLL, which `--relative-address` asks for. Without it, the tool takes the address as one in the DLL loaded at its preferred base, `0x180000000`. It prints the function and the file and line it lies in:

```powershell
llvm-symbolizer --obj=mitto_core.dll --relative-address 0x3c44
```

Measured for Chromium 157.0.8103.39, in MB of 1,000,000 bytes:

| | Linux x86_64 | macOS arm64 | Windows x64 |
| --- | --- | --- | --- |
| Tag | `py3-none-manylinux_2_26_x86_64` | `py3-none-macosx_13_0_arm64` | `py3-none-win_amd64` |
| Wheel | 6.1 MB | 5.5 MB | 6.7 MB |
| Library, installed | 12.3 MB | 10.8 MB | 13.4 MB |
| Symbols archive | 12.8 MB | 11.5 MB, or under 1 MB without the object files | 16.6 MB |

The tag follows from Chromium's build. Chromium 157 sets `mac_deployment_target` to 13.0 in `build/config/mac/mac_sdk.gni`. Its Linux sysroot is Debian bullseye, and `build/linux/sysroot_scripts/reversion_glibc.py` rewrites the sysroot's glibc so that no symbol needs a version above 2.26. The library needs 2.25, for `getentropy` and `getrandom`, and auditwheel has no policy for 2.25, so the tag says 2.26. A Windows wheel's tag names only the CPU, as Windows has no version in a tag. [`tests/test_wheel.py`](../tests/test_wheel.py) keeps the tag equal to what the library needs: what `auditwheel show` says on Linux, the `minos` that `otool -l` shows on macOS, and the machine in the DLL's PE header on Windows.

A wheel with the Page program is tagged for the highest version that any of its binaries needs. On Linux the Page program needs glibc 2.27, for `memfd_create()`, which V8 calls in `v8/src/base/platform/platform-posix.cc`, and which `reversion_glibc.py` leaves at 2.27 on purpose, in its `SYMBOL_ALLOWLIST`. So CI's Linux wheel is tagged `manylinux_2_27_x86_64`, and a release that ships the Runtime will need glibc 2.27 too, unless the Page program stops needing it. On macOS and Windows its tag is the library's. Measured for Chromium 157.0.8103.39, a wheel with the Page program, as CI builds it, is 22.2 MB on Linux, 20.8 MB on macOS and 23.7 MB on Windows, and its symbols archive 33.2 MB, 42.4 MB and 57.6 MB.

To check a wheel, install it into a new environment and run the tests there, with `MITTO_WHEEL` naming the wheel, `MITTO_HOST_NOTICES` naming what the [notices script](#third-party-notices) prints for the library's build, and `MITTO_NATIVE_LIBRARY` unset. Print the notices first, because `uv run` would install the project into the new environment:

```bash
uv run python -m tools.workspace.notices host /absolute/path/to/chromium-<tag>/src/out/mitto > dist/host-notices.txt
export UV_PROJECT_ENVIRONMENT=/absolute/path/to/new/environment
uv sync --no-install-project
uv pip install --python "$UV_PROJECT_ENVIRONMENT/bin/python" dist/mitto-*.whl
MITTO_WHEEL="$(ls "$PWD"/dist/mitto-*.whl)" MITTO_HOST_NOTICES="$PWD/dist/host-notices.txt" "$UV_PROJECT_ENVIRONMENT/bin/python" -m pytest
```

On Windows the environment's Python is `Scripts\python.exe` instead of `bin/python`.

The suite then runs against the installed package, and `MITTO_WHEEL` adds the tests of the wheel itself: that the installed package loads the wheel's library, that the tag is what the library needs, that the symbols archive belongs to the library and names a function that the stripped library does not, that the wheel holds the notice of every project that its library links on this Host, and that it holds a Page program only if `MITTO_PAGE_PROGRAM` names the one installed from it.

### Third-party notices

The library links Chromium and about 80 projects that Chromium uses, such as BoringSSL, QUICHE, ICU, zlib, Brotli, zstd, Abseil, Perfetto, Rust crates and the Rust standard library. Their licences ask that a binary carries their notices, so [`THIRD_PARTY_NOTICES`](../THIRD_PARTY_NOTICES) holds an entry for each project, with the text of its licence, and every wheel carries that file.

[`tools/workspace/notices.py`](../tools/workspace/notices.py) writes it from the library's build graph, as Chromium's `tools/licenses/licenses.py` does for Chrome. The source export does not hold that script, and the script would also count the build's tools:

1. `gn desc` describes every target of the build graph, in a directory of its own with the build directory's `args.gn`, so that the build directory stays as it is.
2. From `//mitto:mitto_core`, it follows the dependencies whose code a shared library links: static libraries, source sets, Rust libraries and groups. An action, an executable and a Rust procedural macro only run during the build, so it stops there. protoc, the program that generates the HSTS preload list and serde's derive macros need no notice.
3. A linked target's code lies in the directories of its sources, or in the target's own directory when all its sources are generated. A Rust crate's code counts as its target's directory, which holds its `README.chromium`.
4. A directory belongs to the nearest project: a directory under a `third_party` directory, or a crate's `third_party/rust/<crate>/<epoch>`, that holds a `README.chromium`. One without, such as a project inside another project, is the nearest directory with licence files, and one with neither belongs to the project around it. Code outside any project is Chromium's, and the library's own code is under `LICENSE`.
5. The `README.chromium` gives each project's name, URL, licence and licence files, with `COPYING` when the files it names do not exist. A dependency marked `Shipped: no` gets no entry: Chromium marks code whose licence another entry already carries that way, such as the part of ICU that `//base` copies. The Rust standard library has no `README.chromium`, and its licence texts come from the Rust toolchain. A linked project without a licence file stops the script.

The Hosts link slightly different projects: only Linux links glog's symbolizer and xdg-user-dirs, and Windows does not link libc++abi or the crates that the Rust standard library reads backtraces with. Linux and Windows link compiler-rt, and only Windows links the code that `//net` takes from Mozilla to read Windows' certificate stores. gn on one Host cannot describe another Host's build, so each entry says which Hosts link its project, and each Host writes its own part, from a build of the [source export](#build-from-the-source-export) or of a [checkout](#build-the-native-core):

```bash
uv run python -m tools.workspace.notices update /absolute/path/to/chromium-<tag>/src/out/mitto
```

It says which entries changed, and drops an entry that no Host links any longer. The native job of each Host in CI keeps that Host's part as the artifact `notices-<runner>` of its run, so one machine can take the other Host's part from there:

```bash
gh run download <run-id> --name notices-ubuntu-latest --dir /absolute/path/to/new/directory
uv run python -m tools.workspace.notices merge /absolute/path/to/new/directory/host-notices.txt
```

Update both parts whenever what the library links may change: when the **Engine Release** moves, in [the release routine](releasing.md#3-make-the-source-export), when `native/BUILD.gn` changes its dependencies, and when `tools/workspace/args.gn` changes. Until a Host's part is right, the test of the installed wheel fails on that Host and names each project that the notices get wrong: one they miss, one they say is linked but is not, and one whose licence text changed.

Measured for Chromium 157.0.8103.39, the notices are 0.6 MB, and they add 0.1 MB to a wheel.

### In CI

The native job of each Host builds a wheel from the library and the Page program that it built or restored, and keeps the wheel and its symbols archive as an artifact of the run. When it builds the library, it also prints this Host's part of the [third-party notices](#third-party-notices) from the same build, and keeps it next to the library in the cache and as an artifact. It installs the wheel into a new environment and runs the tests as above, with `MITTO_PAGE_PROGRAM` naming the Page program in the installed package, so that [`tests/test_page_program.py`](../tests/test_page_program.py) runs the one from the wheel. Building the wheel takes 12 seconds on Linux, 20 on Windows and 11 on macOS, or 50 when `dsymutil` reads the object files of a build that the job made, and keeping and installing it a few seconds more.

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

## The release workflow

[`.github/workflows/release.yml`](../.github/workflows/release.yml) builds the wheels of a release on the hosted runners that CI uses, and publishes them. A tag `vX.Y.Z` on `main` starts it, in [the release routine](releasing.md#11-the-tag). Its jobs run in this order, and each starts only once those before it have passed:

1. **Check what to publish.** The tag must be `v` and the version in `pyproject.toml`, and its commit must be on `main`. `uv run python -m tools.release version --tag vX.Y.Z` makes the first check and prints the version. PyPI must not have that version yet, because an index never takes a version twice. The job also writes the notes of the GitHub release with `uv run python -m tools.release notes X.Y.Z`. They name the **Supported Window**, the **Engine Release**, the Chromium build of the engine, and the build of Chrome that each **Persona** presents, and hold [what changes for callers](releasing.md#what-changes-for-callers) in that version, from `CHANGELOG.md`. They refuse a version with no entry there, and an engine built from another milestone than the Engine Release's. The run's summary shows them.
2. **Build and test**, on `ubuntu-latest`, `xcode-27` and `windows-2025-vs2026`. The Windows job first installs the Windows SDK as CI's does ([In CI](#in-ci)), and runs its steps in Git's bash. Each job [builds the native core from the source export](#build-from-the-source-export) that the repository pins, [builds the wheel](#build-a-wheel) and its symbols archive from the library in its build directory, and prints this Host's part of the [third-party notices](#third-party-notices). It then installs the wheel into a new environment and runs every test against it, with `MITTO_WHEEL` and `MITTO_HOST_NOTICES` set, as above. It builds from nothing: no job of the workflow restores or saves an Actions cache, setup-uv's included, so nothing that another run left can get into a release. The macOS symbols archive therefore keeps its line numbers. It builds neither the Runtime nor a wheel with the Page program: it does not set `MITTO_WHEEL_PAGE_PROGRAM`, and the test of the installed wheel checks that the wheel holds no Page program ([Build a wheel](#build-a-wheel)).
3. **Publish**, from a job in the `pypi` environment, which allows only tags `v*` and waits for the owner's approval. So the owner approves once every build and test has passed, and nothing is public before. It is the only job with `id-token: write`, so no other job can get the OIDC token that Trusted Publishing uploads with, and PyPI takes uploads to `mitto` from this workflow in this environment only. It uploads the three wheels and nothing else: there is no sdist, and the symbols archives are not for PyPI.
4. **GitHub release** of the tag, with the three wheels, the three symbols archives, a `SHA256SUMS` file of the six, and the notes of the first job.
5. **Install**, on each Host, at the same time as the GitHub release: `pip install mitto==X.Y.Z` from PyPI into a new environment of the newest Python that mitto supports, and a request with it to a local server. It tries for five minutes, because an index can take a while to list a new version.

A tag that fails the first job has published nothing. Delete it, fix what the job says, and tag again. Once the wheels are on PyPI, rerun what failed with `gh run rerun <run-id> --failed` rather than tagging again, because PyPI would refuse the version. That also finishes a publish that stopped after one wheel: the rerun sends the same three wheels, and PyPI takes a file that it already has, byte for byte, as it takes a new one. It refuses another file of the same name. A macOS job that GitHub cancels after about 15 minutes, with no runner named, has not started: rerun it the same way.

In the first dry run, on 2026-11-07, the whole run took 33 minutes. The build jobs took 27 minutes on macOS and 32 on Linux, of which the native core took 18 and 24 minutes and the tests about 8 minutes on either Host. Each later job took under half a minute.

In the first dry run with Windows, on 2026-11-09, the build jobs took 24 minutes on macOS, 28 on Linux and 36 on Windows, of which the native core took 15, 19 and 24 minutes, the tests about 9 minutes on each Host, and the install of the Windows SDK 2 minutes. CI's two builds from nothing on Windows had taken 40 and 41 minutes. Each later job took under half a minute. The Linux and Windows jobs needed two reruns with `gh run rerun <run-id> --failed`, because GitHub answered 504 to the download of the source export during an incident on its side, and their last attempt took 37 minutes from start to the end of the run.

### A dry run

Run by hand, the workflow is a dry run. It publishes a pre-release to TestPyPI from the `testpypi` environment, which waits for nobody, and makes no public GitHub release. For example, from `main`:

```bash
gh workflow run release.yml --ref main -f version=0.1.0rc1
```

It does what a release does, with these differences:

- The version comes from the input, and must be a pre-release in PEP 440's normal form, such as `0.1.0rc1` or `0.1.0.dev1`. Each build job writes it into its own checkout's `pyproject.toml` and `uv.lock` with `uv version`, so the branch keeps its version. TestPyPI never takes a version twice, even once it is deleted, so each dry run needs a new one.
- No tag is checked, so a dry run can run from any branch. A run by hand is a dry run even from a tag.
- Its notes hold the entry in `CHANGELOG.md` of the release that the pre-release comes before, such as 0.2.0 for `0.2.0rc1`, so its summary shows the notes of that release.
- It publishes to TestPyPI, and installs from `https://test.pypi.org/simple/`.
- The GitHub release is a draft named `dry-run-<version>`, which only those who can write to the repository see, and the job deletes it again once its assets are up. A draft makes no tag.

GitHub only offers a run by hand of a workflow that is on the default branch.

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

On Linux the file is `libmitto_core.so`. On Windows, in PowerShell:

```powershell
$env:MITTO_NATIVE_LIBRARY = "$env:MITTO_WORKSPACE\chromium-<tag>\src\out\mitto\mitto_core.dll"
uv run pytest
```

The tests of the asyncio API go through each way that an event loop can hear of a Session's events on the Host: on macOS and Linux by watching the Session's file descriptor and through the thread that Windows has instead, and on Windows on `ProactorEventLoop` and on `SelectorEventLoop`.

Those tests include the **Parity Checks**, described in [parity.md](parity.md).

Tests of how the loader rejects a library compile small stub libraries with `cc`, and are skipped when there is no C compiler. A new Ubuntu install has none, and the native core's build does not need one. Install `gcc` to run these tests. Windows has no `cc` of its own, so they are skipped there unless one is on `PATH`, such as MinGW's on GitHub's Windows runners. The tests of the wheel's build hook do not compile a stub on Windows, because a DLL from `cc` has no PDB: they build a wheel from a made-up DLL and PDB instead, on every Host.

To check the whole path from Python through the native core to a real server, with `MITTO_NATIVE_LIBRARY` set as above and a network connection:

```bash
uv run python -c "import mitto; print(mitto.get('https://example.com/').status)"
```

It prints `200`.

The loader in [`src/mitto/_native.py`](../src/mitto/_native.py) uses `MITTO_NATIVE_LIBRARY` if it is set, and otherwise `libmitto_core.dylib`, `libmitto_core.so` on Linux or `mitto_core.dll` on Windows, next to that file in the installed package. If the variable names something that is not a file, loading fails rather than falling back to the installed library. The loader refuses a library whose ABI version differs from its own `ABI_VERSION`, and a test keeps that constant equal to `MITTO_ABI_VERSION` in the C header.

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

On Windows x64, for Chromium 157.0.8103.39, the workspace takes 25.6 GiB after building the native core: 22.9 GiB for `chromium-<tag>/src` without `out`, in about 1.07 million files, 1.3 GiB for `out/mitto`, 0.8 GiB for `depot_tools` and 0.6 GiB for `.cache`. Visual Studio and the Windows SDK come on top, on the system disk (see [Requirements](#requirements)).

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

### The files mitto ports

Some of mitto is ported from Chromium rather than built from it: code that the source export leaves out, such as Blink's and the browser process's, and code whose build target brings in too much, such as the network service's. Each file in `native/` and `src/mitto/` that ports code or copies values names each file they come from, by its path in Chromium's `src`, on a line of its own beside its prose: `// Upstream: services/network/sec_header_helpers.cc` in C++, and `# Upstream: <path>` in Python and GN. Code from a project inside the tree, such as `third_party/rapidhash`, gets the line too. A file that only calls Chromium's code, the way Chrome does, names nothing. A test checks that every file whose comments say it is ported, or a port, has such a line, and that every line that is meant as one is in this form. CI has no checkout, so the test does not check the paths.

```bash
export MITTO_WORKSPACE=/absolute/path/to/mitto-workspace
uv run python -m tools.workspace.upstream 156.0.8078.80 157.0.8103.39
```

The script collects those paths, runs `tools/workspace/compare` on them, and then prints the diff of each one between the two tags, with the files that port it. It fails if a path is not a file at either tag, as when upstream moves a file, and before it compares anything if a file that says it is ported names nothing or a line is not in the form. `compare` is a bash script, so this runs on macOS or Linux. From 156.0.8078.80 to 157.0.8103.39, 19 of the 56 files changed, and none of the changes was one that a port had to follow.

## Make the source export

[`tools/workspace/export.py`](../tools/workspace/export.py) makes the source export by hand, on machines that hold a workspace. Make a new one when:

- the **Engine Release** moves, in [the release routine](releasing.md);
- a patch in [`tools/workspace/patches`](../tools/workspace/patches) or a value in [`tools/workspace/args.gn`](../tools/workspace/args.gn) changes;
- a new build configuration, such as another sanitizer, needs files that this one does not;
- `native/page` or `native/bindings` starts to depend on a target outside the Runtime's build graph, as for `native/` below;
- `native/` starts to depend on a target outside the current build graph. Until then, `native/` can include any header that a target in the graph declares, because the export keeps those even when nothing includes them yet. A build that needs something else stops with a missing file.

What a build reads differs per Host, and `gn` on Linux knows nothing of the inputs of macOS or Windows, so each Host contributes a **slice**. A slice covers every build configuration that the Host has: the plain one and the Runtime's, `runtime`, on every Host, and also `asan`, `ubsan`, `fuzz`, `runtime-asan` and `runtime-ubsan` on Linux. The last two were added after `mitto-chromium-157.0.8103.39-3.tar.xz`, whose Linux slice did not trace them, and they build from it all the same. The steps took about three and a half hours on 2026-11-10, on machines that had built the native core and the Runtime before, and most of it on the machine that runs Linux and Windows in turn. The slices took 36 minutes on Linux for its five builds, 29 minutes on macOS with `-j 5` and 33 minutes on Windows. The builds from the archive in step 4 took 48 minutes on Linux, 29 minutes on macOS and 33 minutes on Windows, before the tests.

1. On each Host, in a workspace [set up by the bootstrap script](#set-up-the-workspace), from the same commit of this repository, with the bootstrap script run again from that commit:

   ```bash
   export MITTO_WORKSPACE=/absolute/path/to/mitto-workspace
   uv run tools/workspace/export.py slice
   ```

   On Windows, in PowerShell:

   ```powershell
   $env:MITTO_WORKSPACE = 'C:\ws'
   uv run tools/workspace/export.py slice
   ```

   This builds each configuration from nothing in `out/export-trace-<config>`, the native core or, for `runtime`, the group `//mitto:runtime`, with depot_tools off the `PATH`, and then copies what those builds used to `$MITTO_WORKSPACE/export/<tag>/<host>`, where `<host>` is `linux`, `mac` or `win`. It removes each build directory afterwards, and `out/mitto` stays as it was. The Runtime's build compiles V8, whose compile jobs need far more memory than the library's, so on a Mac with 16 GB add `-j 5`, as for [the Runtime](#build-the-runtime) in a checkout: it sets how many build steps ninja runs at once, which by default is the number of cores plus two.

2. Copy the other two slices to the same place on the Linux or the macOS machine, for example with `rsync -az`, or from Windows with its `tar` and `scp`.

3. There, merge the slices:

   ```bash
   uv run tools/workspace/export.py merge
   ```

   This writes `$MITTO_WORKSPACE/export/<tag>/mitto-chromium-<tag>.tar.xz` and prints its size, its hash and its contents per directory. The same slices give the same bytes, on macOS as on Linux.

4. On each Host, [build from that archive](#build-from-the-source-export) with `--archive` in a new directory, once with each `--config` that the Host has, and run the tests. The build must not find its way back to the checkout: keep depot_tools off the `PATH` and `MITTO_WORKSPACE` unset. The command must also say each time that the library is the same as the one the slice built, or for `runtime` that the Page program is. A difference means that a file the export left out changed the result without stopping the build, for example through `path_exists()` in GN or `__has_include` in C++. On Linux the comparison covers the whole file. On macOS it covers the file's sections, because the rest holds the absolute paths of the object files. On Windows it covers the whole DLL except the 16 bytes that name its PDB, the signature in its CodeView debug record: the linker hashes that signature from the PDB, which holds the absolute paths of the build. A DLL built in another directory differs from the slice's in those 16 bytes alone.

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
- every file below a directory of the source tree that compilers search with `-isystem`, or `-imsvc` on Windows, because `-MMD` and `/showIncludes:user` leave those headers out of what they report;
- every file that a build script's Python opened or got as an argument, recorded through Python's audit events, which see this on every Host without root;
- `DEPS`, and `build/toolchain/win/rc/win/rc.exe.sha1`, the SHA-1 by which a hook of `DEPS` downloads `rc.exe`, both of which the merge reads;
- the `BUILD.gn` of the Host's Clang modules, which `gn` only checks with `path_exists()`: `build/modules/linux/BUILD.gn` on Linux and `build/modules/win/BUILD.gn` on Windows, under the name that `gn` writes into `gen/module_platform.txt`. macOS has none;
- the licence files that apply to all of the above: `LICENSE`, `COPYING`, `NOTICE`, `AUTHORS` and similar names in each directory on the way up to a kept file, and `README.chromium` with the file its `License File:` field names.

It leaves out the build output, `//mitto` and the toolchain. It stops if it would keep a prebuilt program, for macOS, Linux or Windows, because that belongs in the toolchain list at the top of the script. Paths in a slice use `/` on every Host. Windows finds a file whatever the case of its path, so a tool there can name a file in another case than its directory does, and a Windows slice names each file as its directory spells it. Visual Studio's and the SDK's headers are system headers there, which the compiler does not report, and they lie outside the checkout in any case.

The merge stops if a path differs between slices, other than `build/config/gclient_args.gni`, which gclient writes per Host and whose `checkout_*` values decide nothing in this build graph. So a file must have the same bytes in every slice, which the bootstrap script's git settings make sure of on Windows. A file that several slices hold comes from the Linux slice, then the macOS one, then the Windows one. Windows keeps no file modes, so a file that only the Windows slice holds goes into the archive as `0644`: only Windows builds with it, and Windows has no use for a mode. The merge also stops if a third-party project in the export has no licence file. It reads the toolchain's pins from `DEPS`, resolves those in CIPD to a hash with depot_tools, and downloads `rc.exe` to check it against its SHA-1 and record its SHA-256.

### Update the export to a patch build

A patch build of the **Engine Release** changes few files of the tree, and seldom what the native core's build reads, so the export of the build before it still names every file that the new build reads, on every **Host**. For a [patch run](releasing.md#a-patch-run), one machine can then update that export from the diff between the two tags, with no checkout of the new build and no slices. [`tools/workspace/update_export.py`](../tools/workspace/update_export.py) does it, on macOS or Linux, in a workspace whose depot_tools the bootstrap script set up, once [`tools/workspace/chromium_tag`](../tools/workspace/chromium_tag) names the new build:

```bash
export MITTO_WORKSPACE=/absolute/path/to/mitto-workspace
uv run python -m tools.workspace.update_export
```

It updates the pinned export, or the one that `--archive` names. It:

1. fetches the two tags of chromium/src at depth 1 into `$MITTO_WORKSPACE/compare/src`, as [`compare`](#compare-two-releases) does, and into `compare/deps` each dependency that `DEPS` moved between them and that the export holds files of, such as V8, BoringSSL or ICU, at the two commits that `DEPS` names;
2. takes each file of the export that git changed from the new tag, in chromium/src or in the dependency that holds it, and applies a patch of [`tools/workspace/patches`](../tools/workspace/patches) again where upstream changed a patched file;
3. writes again the files that hooks of `DEPS` write, not git: `build/util/LASTCHANGE.committime`, which holds the new build's commit time, and, when V8 moved, V8's builtins profiles for the new V8's version, from Chromium's storage;
4. pins the toolchain of the new `DEPS`, as the merge does;
5. writes `$MITTO_WORKSPACE/export/<tag>/mitto-chromium-<tag>.tar.xz`, with the same files as the old export, and prints its size, its hash and the files that changed.

It stops before it writes anything, and the export then needs slices as above, when the diff:

- deletes a file of the export;
- changes a build file of the export, a `BUILD.gn`, another `.gn` or `.gni` file or a `.pydeps` list, which can add a file to the build or a target to its graph;
- adds to a file of the export an `#include`, `#import` or `__has_include` of a header that no file of the export ends with, other than a generated one, or an import to a Python script;
- changes a patched file so that its patch no longer applies, which needs the patch updated as in a run;
- is between builds of different branches, such as of two milestones.

Those are the ways in which a build can come to read a file that the export does not hold, on any Host. The rule is stricter than it needs to be: a change to a `BUILD.gn` that only changes a flag stops it too.

The archive's `export.json` names the export it came from, under `updated_from`, and holds no fingerprints of traced builds, since no slice built anything. So a build from it says that it has nothing to compare its library with. [Build from that archive](#build-from-the-source-export) on the machine, with `--archive` and no checkout, once with each `--config` that it has, and let CI build it on every Host, as for any export.

On 2026-11-10 this updated the export of 157.0.8103.27 (`mitto-chromium-157.0.8103.27-2.tar.xz`) to 157.0.8103.39 in 2 minutes on the Mac, with both tags already in `compare/src`. Upstream had changed 7 of its files: five of the HTTP cache, the fix for a 206 that overruns its range, as well as `DEPS` and `chrome/VERSION`. Of the dependencies, only ANGLE had moved, which the export holds 5 files of, licences and a `.gni` file, none of them changed. All 12,380 files under `src/` of the result had the same bytes, modes and times as in the export that slices made of 157.0.8103.39, `mitto-chromium-157.0.8103.39.tar.xz`, and so did the toolchain's pins. Only `export.json`, with its `updated_from` and no fingerprints, differed, and the README, whose text names Windows since the export of 157.0.8103.39 that added its slice.

With the Runtime in the export, a patch build changes V8 too. The same day, the script took `mitto-chromium-157.0.8103.39-3.tar.xz` to 157.0.8103.27, the other way through the same diff, in 3 minutes. 32 of its files changed: 23 of V8, from ten of its rolls, Blink's `runtime_enabled_features.json5`, the HTTP cache's five, `DEPS` and `chrome/VERSION`. None was a build file or added an include, and it fetched the builtins profiles of V8 15.7.112.12. On the Mac the result built the library and the Runtime with no checkout, and that `mitto_page` printed `15.7.112.12` for `--version`. No export of 157.0.8103.27 that carries the Runtime exists to compare that with.
