# Releasing

Each mitto release ships one engine, built from the **Engine Release**, and presents every **Chrome Release** of the **Supported Window** as **Persona** data on it ([ADR-0005](adr/0005-older-releases-as-data.md)). Chrome ships a new stable milestone every few weeks, and the routine on this page moves mitto with it. It lists every step in order, with the machine it runs on and whether it needs the owner's OK. Terms in **bold** are defined in [CONTEXT.md](../CONTEXT.md).

## When a run starts

A run starts when a milestone reaches its stable date in [Chromium Dash](https://chromiumdash.appspot.com/schedule), not its early stable date. About two weeks before the stable date, the early stable date gives the milestone to a small share of users only, and most installations are still on the one before. The run makes that milestone the Engine Release, records every Chrome Release of the new Supported Window, drops the oldest, and publishes.

A run raises the minor version: 0.1.0, 0.2.0 and so on. Between two runs, a change that every caller can take as it is raises the patch, or waits for the next minor version. [A patch run](#a-patch-run) says when a security fix in a patch build of the Engine Release gets a patch of its own. While mitto is at 0.x, a change that is not compatible, such as one whose files an older mitto cannot read, may also ship in a minor version of its own, without waiting for a run. Either goes through the [last three steps](#10-the-version) only, and the next run still raises the minor version by one. Every version is made from `main`, so a patch only follows the newest minor version, and no patch of an older one ships. How versions are numbered from 1.0 on is decided when 1.0 comes. Dropping a Chrome Release, and with it its Persona, only happens in a run.

## The steps

| Step | Where | Who approves |
| --- | --- | --- |
| 1. [Find the builds](#1-find-the-builds) | Any machine | Nobody |
| 2. [Check out the new Engine Release](#2-check-out-the-new-engine-release) | The workspace's macOS, Linux and Windows machines | Nobody |
| 3. [Make the source export](#3-make-the-source-export) | The workspace's macOS, Linux and Windows machines | The owner, before it is published |
| 4. [Record Baselines](#4-record-baselines), with the Platform Runtime component | A Windows machine | Nobody |
| 5. [Check for Merkle Tree Certificates](#5-check-for-merkle-tree-certificates) | The workspace, the Windows machine and any machine | The owner, if a seed turns them on |
| 6. [Write the Persona data](#6-write-the-persona-data) | Any machine | Nobody |
| 7. [Refresh the table of Machines](#7-refresh-the-table-of-machines) | Any machine | Review of the run's pull request |
| 8. [Drop the oldest Chrome Release](#8-drop-the-oldest-chrome-release) | Any machine | Nobody |
| 9. [The known-gaps list](#9-the-known-gaps-list) | A macOS or Linux **Host** | The owner, for any gap that stays |
| 10. [The version](#10-the-version) | Any machine | Review of the run's pull request |
| 11. [The tag](#11-the-tag) | Any machine | The owner |
| 12. [The publish](#12-the-publish) | GitHub's hosted runners | The owner, in the `pypi` environment |

"Any machine" is one with the repository and uv. The scripts of steps 1, 6, 7 and 8 need no native library. Steps 2 to 10 end in one pull request, and CI builds it from the new export on its macOS, Linux and Windows runners.

### 1. Find the builds

```bash
uv run python -m tools.release builds
```

This prints the next Supported Window: the milestone after the newest Persona, as the Engine Release, and the two before it. From Chromium Dash it takes the Engine Release's stable date and early stable date. From [Google's version history](https://versionhistory.googleapis.com/v1/chrome/platforms/win64/channels/stable/versions/all/releases?filter=endtime=none) it takes the newest Windows stable build of each Chrome Release that can be pinned, and prints it beside the build of the Chrome Release's Baselines. Windows is used because every Persona is Windows Chrome. `--engine RELEASE` names another Engine Release, such as during a run, once its Persona exists.

Each Chrome Release is recorded at its newest pinnable build, because that is the one Google's update server gives for it ([Install Chrome](baselines.md#install-chrome)). A newer build in a partial rollout cannot be pinned, and is not recorded. Each run records the older Chrome Releases again at their newest pinnable builds, because `sec-ch-ua-full-version-list` names the build. An Engine Release with no pinnable build yet has `none yet`, and the run waits for one.

### 2. Check out the new Engine Release

1. Write the new Engine Release's newest pinnable build from [step 1](#1-find-the-builds) into [`tools/workspace/chromium_tag`](../tools/workspace/chromium_tag). Its checkout and its Baselines take the same build, so that the engine runs the network code that was recorded.
2. Rerun [`tools/workspace/bootstrap.py`](building.md#set-up-the-workspace) on each of the workspace's three machines: the macOS, the Linux and the Windows machine. It creates `$MITTO_WORKSPACE/chromium-<tag>` for the new release, with its own checkout and `out/mitto`. If a patch no longer applies, the script stops after the sync. Update the patch against the new checkout and rerun the script. Delete a patch the new release no longer needs.
3. [Build the native core](building.md#build-the-native-core) in the new checkout on each machine. On the Windows machine, check first that Visual Studio and the Windows SDK are the versions that the new release asks for, in its `docs/windows_build_instructions.md` and `build/vs_toolchain.py`. They are the machine's and not the checkout's, and a build stops before it starts without that SDK.
4. [Compare](building.md#compare-two-releases) the build of each older Chrome Release in the new Supported Window with the new one. A change in the network code that sets up a connection is one that Persona data may have to undo for the older Chrome Releases ([what differs between Chrome Releases](persona.md#what-differs-between-chrome-releases)). It is a decision for the milestone's plan.
5. On the macOS or the Linux machine, [compare the files that mitto ports](building.md#the-files-mitto-ports), from the build that `tools/workspace/chromium_tag` named before this run, as `git show main:tools/workspace/chromium_tag` prints it, to the new one:

   ```bash
   uv run python -m tools.workspace.upstream OLD_BUILD BUILD
   ```

   Review the diff of each upstream file. A change that a port must follow goes into the run's pull request. One that it need not follow is written down there, with why. If the script stops because a file is missing at the new build, find where its code went, and move the `Upstream:` line with the port.
6. Once the new release works, delete the old release's `chromium-<tag>` directory on each machine.

Each release has its own checkout because a shallow checkout cannot move to a new release in place: gclient would fetch every branch with full history. Until the last step, the old checkout and its build output stay usable.

### 3. Make the source export

[Make the source export](building.md#make-the-source-export) of the new release, with a slice from each of the workspace's three machines, publish it with the owner's OK, and write its URL and SHA-256 into [`tools/workspace/source_export.json`](../tools/workspace/source_export.json). CI and the release workflow then build the native core from it. The Windows machine's slice and its build from the archive take the Visual Studio and the Windows SDK that [step 2](#2-check-out-the-new-engine-release) checked: they stay the machine's and are not in the export, and a build from the export stops before it starts without that SDK. The Windows jobs of CI and of the release workflow install the SDK that [`.github/actions/install-windows-sdk/action.yml`](../.github/actions/install-windows-sdk/action.yml) pins, so a new SDK version goes there too, with its installer and that installer's SHA-256.

On each of the three machines, [update its part of the third-party notices](building.md#third-party-notices) from a build of the new export, such as the one that checks the archive, and commit the result with the run's pull request. CI's test of the installed wheel fails on a Host whose part is wrong, and so does the release workflow's. A Host's part can also come from the run of that pull request: [merge](building.md#third-party-notices) the notices that CI's job of that Host keeps.

From the same build on the macOS or the Linux machine, check [ICU's data](building.md#icus-data) again. Disassemble the unstripped library with `objdump -d`, and find every call to ICU's entry points to its data, whose names end in the ICU version, such as `ures_openDirect_78`. A caller that the table of that section does not name, or a new caller of one that passes a name on, such as `base::CodepageToUTF16()`, may ask for an item that the library lacks. Read what it asks for, and add the item to [`native/make_icu_data.py`](../native/make_icu_data.py). The build stops if an item in that list is missing from ICU's new data.

### 4. Record Baselines

On the Windows machine, [record Baselines](baselines.md) of every Chrome Release in the new Supported Window: the Engine Release [installed](baselines.md#install-chrome), and the older ones [unpacked](baselines.md#an-older-chrome-release), each at the build that the update server gives. [Check and commit](baselines.md#check-and-commit) them, update [where each Chrome Release comes from](baselines.md#where-each-chrome-release-comes-from), and write down what the recording found under its Chrome Release on that page.

Then check for the Platform Runtime component, which [no Chrome has shown so far](capture.md#what-this-does-not-cover-1). Start Chrome of each Chrome Release from a new user data directory with [the first five flags](baselines.md#what-it-does), leave it for 15 minutes, and open `chrome://components`. Write down whether it lists Platform Runtime, and whether the user data directory has a `PlatformRuntime` directory. If one does, the component reaches Chrome, and by Chrome's source its library can change the lines Chrome adds for [Google's domains](capture.md#what-chrome-adds-for-googles-domains). That is a decision for the milestone's plan.

### 5. Check for Merkle Tree Certificates

With `VerifyMTCs` on, Chrome adds the IDs of its Merkle Tree CAs to `trust_anchors` ([what this does not cover](capture.md#what-this-does-not-cover)). [`native/feature_list.cc`](../native/feature_list.cc) states it off, so a Session would not follow. When the Engine Release moves, check three places:

- In the new checkout on the workspace, the default of `kVerifyMTCs` in `net/base/features.cc`.
- On the Windows machine, the variations seed of each recording. `chrome://version/?show-variations-cmd` of a recording start must not name `VerifyMTCs`, and neither may the seed itself, `VariationsSafeSeedV2` in the user data directory, once decompressed ([how to read it](baselines.md#the-variations-seed-and-the-country)). `record.ps1` keeps a feature or field trial in `chrome.txt` when its name has `mtc` in it, so `chrome.txt` shows one that a recording drew.
- In the new Baselines, `trust_anchors` in each ClientHello. The Parity Checks of [step 9](#9-the-known-gaps-list) show a gap there if Chrome sent more IDs than the compiled-in root store has.

Write the answer down under the recording in [baselines.md](baselines.md). If any of the three shows `VerifyMTCs` on, what a Session sends is a decision for the milestone's plan.

### 6. Write the Persona data

For each Chrome Release whose Baselines are new:

```bash
uv run python -m tools.release persona RELEASE
```

This writes `src/mitto/personas/chrome_RELEASE_windows.json` from `tests/baselines/RELEASE`, [as persona.md describes](persona.md#write-one). For the new Engine Release, also name its Persona in [`src/mitto/_persona.py`](../src/mitto/_persona.py): a constant like the others, its entry in `__all__` and in `WINDOWS_PERSONAS`, and `ENGINE_RELEASE_PERSONA`. The tests that check what a Session presents by default name the Engine Release, and fail until they move with it. The README's line follows in [step 8](#8-drop-the-oldest-chrome-release).

Then run `uv run pytest tests/test_persona.py tests/release`. If the script refuses, for example because a Content-Type is not the one it knows, Chrome has changed what a request sends. Change the script and [persona.md](persona.md) to say what it does now.

### 7. Refresh the table of Machines

```bash
uv run python -m tools.release machines
```

This fetches the public statistics that [the table of Machines](persona.md#the-table) is made from again, writes them to [`tools/release/machine_statistics.json`](../tools/release/machine_statistics.json), and writes [`src/mitto/machine_table.json`](../src/mitto/machine_table.json) from them. It prints the period it took from each source, and whether that is new, revised or as before. Then read the diff:

```bash
git diff -- tools/release/machine_statistics.json src/mitto/machine_table.json
```

| Source | How often it updates | What a refresh takes |
| --- | --- | --- |
| StatCounter: Windows 10 against 11, and the display in CSS pixels | Monthly. A month comes out on the second of the next, and StatCounter revises it for 45 days from then | The newest month it no longer revises, so a month counts from the middle of the month after next. It asks the CSV link that StatCounter's pages use for their download, which StatCounter does not document |
| Firefox Public Data Report: the display in pixels, and the memory | Weekly, at stable JSON addresses under `data.firefox.com/datasets/desktop/hardware` | Its newest week |

The statistics' diff shows which shares moved, and the table's what that does to the weights. The table's credits name the period of each source, so they follow what was fetched, and [`MACHINE_TABLE_NOTICE`](../MACHINE_TABLE_NOTICE) leaves the periods to them. The owner's weights, which the table marks, stay as they are: they are in [`tools/release/machines.py`](../tools/release/machines.py), and only a change there moves them. A name that a source did not give before, such as a new version of Windows or a new display size, counts only where [persona.md](persona.md#the-table) says the table takes it, so look for one in the diff.

If a source does not answer, or answers in a form the script does not read, such as a CSV of another month or JSON without its shares, the script says which and why, exits with an error and changes neither file. The table then stays as it was for this run: say so in the run's pull request. A form that has changed for good is a change to [`tools/release/machine_sources.py`](../tools/release/machine_sources.py) and to the answers saved in `tests/release/data`, which `tests/release` checks it with, and no network.

`--saved` writes the table from the statistics as they are and fetches nothing, such as after a change to an owner's weight.

### 8. Drop the oldest Chrome Release

```bash
uv run python -m tools.release drop
```

This deletes the oldest Persona's file, its Baselines and its entries in the known-gaps list, and takes its name out of `src/mitto/_persona.py`. It also writes the line of [`README.md`](../README.md) that names the Supported Window, from the Personas that are left and `ENGINE_RELEASE_PERSONA`, in the words that `tests/test_docs.py` checks. It refuses while there are only three Personas, so it only runs once the new Engine Release has one. It also refuses, without changing anything, if `_persona.py` names the oldest anywhere else, if the Engine Release is not the newest Persona, or if the README has no such line. It then prints any other line of code that still names it, by its Persona, a build or its `User-Agent`. A line in `tests/release` that holds what Chromium Dash said on one day is a record and stays.

Then go through the docs with the [rule below](#lines-that-name-a-chrome-release), with the dropped Chrome Release in place of `RELEASE`:

```bash
git grep -n -w RELEASE -- docs
```

### 9. The known-gaps list

On a macOS or Linux Host, [build the native core from the new export](building.md#build-from-the-source-export), then [write the known-gaps list again](parity.md#the-known-gaps-list) with `MITTO_NATIVE_LIBRARY` set, and read its diff. The Parity Checks now run every Persona on the new engine, so a gap of an older Chrome Release that the run brought is network code that moved. It goes back into Persona data, as [persona.md](persona.md#what-differs-between-chrome-releases) says how, or the owner decides to keep it. Write down what differs between the Chrome Releases of the new Supported Window on that page, as a new record.

### 10. The version

Raise `version` in [`pyproject.toml`](../pyproject.toml) and run `uv lock`, which writes it into `uv.lock` too. Then read the notes of the version's GitHub release, which hold [its entry in `CHANGELOG.md`](#what-changes-for-callers):

```bash
uv run python -m tools.release notes X.Y.Z
```

A run adds to the entry what it changes for callers itself, such as the Persona that [step 8](#8-drop-the-oldest-chrome-release) dropped, whose name `persona=` refuses from then on. Without an entry for the version, the command stops and says so, and so do `tests/release` and the first job of the release workflow. The run's pull request then holds everything from step 2 on.

### 11. The tag

Once the pull request is merged, tag its commit on `main` as `vX.Y.Z`, with the version of step 10, and push the tag, with the owner's OK. The tag starts the publish.

### 12. The publish

The tag starts [the release workflow](building.md#the-release-workflow), `.github/workflows/release.yml`. It checks that the tag is the version in `pyproject.toml` and that its commit is on `main`, builds the wheels from nothing, with no Actions cache, on the hosted runners that CI uses, and runs every test against each installed wheel. It then waits for the owner's approval in the `pypi` environment: approve the run's deployment once the three builds have passed. It uploads the wheels to PyPI with Trusted Publishing. No API token can upload to `mitto`. The symbols archives go to the GitHub release of the tag, not to PyPI. There are wheels only, and no sdist. Last, it installs the version from PyPI on each Host and makes a request with it.

A [dry run](building.md#a-dry-run) does the same with a pre-release on TestPyPI, and needs no tag and no approval.

## A patch run

Between two runs, Chrome ships patch builds of each milestone, and many of them fix security bugs. Some fix code that mitto ships and that reads what servers send, such as `//net`, BoringSSL, QUICHE, and the parts of `//base` and ICU that the native core links, and V8 once the **Runtime** ships. A patch run moves the engine to such a build of the **Engine Release**, and ships a patch version.

### When one starts

The owner chose this on 2026-11-10, in [#188](https://github.com/darionxt/mitto/issues/188):

- By default, none starts. A fix in a patch build reaches mitto with the next run, whose [first step](#1-find-the-builds) takes the newest pinnable build of the next milestone, which carries the same fix. 157.0.8103.39 carried the fix for a 206 that overruns its range, which 156.0.8078.79 shipped first.
- The owner may start one when the newest pinnable build of the Engine Release carries a fix, in a file of the source export, that the release notes rate Critical or say is exploited in the wild. It is the owner's call each time, and nothing promises one.
- It has no deadline, and follows Chrome's build when the owner has the time.

### Find what a build changes

Run this after the [Chrome Releases](https://chromereleases.googleblog.com/) blog posts a stable update for desktop of the Engine Release:

```bash
uv run python -m tools.release patch-builds
```

It lists each Windows stable build of the Engine Release after the build in [`tools/workspace/chromium_tag`](../tools/workspace/chromium_tag), from Google's version history, partial rollouts included, with the day it first went out and whether it can be pinned. For each one it gives the commits since that build that change files of the [source export](building.md#make-the-source-export), and the severest fix that the release notes list for them. Its last line says whether a build carries a fix for which the owner may start a patch run. `--since BUILD` and `--until BUILD` look at other builds of the same milestone, and `--archive PATH` reads a local copy of the export instead of downloading the pinned one, which is 25 MiB. It needs the network, and no workspace, and took about 20 seconds.

The bugs behind a fix stay restricted until most users have it, but what changed does not. Each build is a tag of Chromium's repository, and the commits between two tags are public on its Gitiles server as soon as the build ships. The command reads them there:

- A file counts when the export holds it. `chrome/VERSION` changes in every build, and `DEPS` with every roll, so neither counts by itself.
- A submodule that the export holds files of, such as BoringSSL in `third_party/boringssl/src`, QUICHE or ICU, moves in a commit that Chromium titles "Roll commits from side projects". The command reads the submodule's own commits between the two that Chromium names, and counts those that change files of the export. It does not read a submodule that the export holds nothing of, such as V8 today.
- It lists apart a commit that changes `DEPS` and rolls no submodule, because one can move the toolchain that the export pins in `export.json`.
- It says when a build lacks a commit of the engine's build that changes exported files. A respin on a branch of its own can: 157.0.8103.50 and 157.0.8103.51 are on a branch from 157.0.8103.38, and lack the one commit of 157.0.8103.39, which only sets its version.
- The release notes list each security fix with the number of its bug and how severe it is, and a commit that fixes it names the same number on a `Bug:` line. The command matches the two, from the blog's posts of about the last two months.

The notes only rate what the commits show. They can list a fix late or leave it out, and they name the component that the bug was filed under, which need not be where the fix is. The commits decide what a build changed.

### What the patch builds of 155 to 157 changed

Measured on 2026-11-10 with the command, against the export of 157.0.8103.39, for each pinnable patch build of a Chrome Release while it was the newest stable one:

| Build | Pinnable from | Days to the next stable date | What it changed in exported files | Fixes in the notes |
| --- | --- | --- | --- | --- |
| 155.0.8059.80 | 2026-10-13 | 7 | `net/socket/transport_client_socket_pool.cc` | Critical CVE-2026-110161, use after free in Network |
| 155.0.8059.85 | 2026-10-15 | 5 | Nothing | |
| 156.0.8078.74 | 2026-10-27 | 7 | The HTTP cache, ICU's `charstr.cpp` and its time zones, `base/metrics`, the epoll pump of `//base`, and the blocklist of certificates | High CVE-2026-118352 in the cache, CVE-2026-118318 in ICU, which the notes file under V8, and CVE-2026-118335 in Metrics. The blocklist, which adds eMudhra's compromised intermediates, is in no notes |
| 156.0.8078.80 | 2026-10-29 | 5 | How the HTTP cache handles a 206 that overruns its range | High CVE-2026-76029, improper input validation in Network |
| 157.0.8103.51 | 2026-11-09 | 8 | Nothing | |

- Three of the five changed exported code, and each of those carried a High or Critical fix. By the owner's rule, only 155.0.8059.80 called for a patch run.
- Each came five to eight days before the next stable date, so the next run followed within about a week.
- The notes of 2026-11-09 list nine fixes for 157.0.8103.50 and 157.0.8103.51. Those builds bring the two in XML. One shipped in 157.0.8103.39 already, and no commit from 157.0.8103.27 to 157.0.8103.51 names the bugs of the other six.
- V8 rolled in four of the five, and the notes listed V8 fixes for each of those four. Once the export holds V8, nearly every patch build will change exported code.

### What a patch run does

It takes the newest pinnable build of the Engine Release that the command names, and goes through fewer steps than a run:

1. Write the build into `tools/workspace/chromium_tag`, as in [step 2](#2-check-out-the-new-engine-release), and [make the source export](#3-make-the-source-export) of it, with the third-party notices and, if the build rolled ICU, a new check of ICU's data. That takes a new checkout and a slice on each of the workspace's three machines, as in a run, until [#178](https://github.com/darionxt/mitto/issues/178) finds out whether a patch build can update the export on one machine, from the diff between the two tags. [building.md](building.md) will then say how.
2. Compare each ported file with its upstream from the engine's old build to the new one, with the upstream check that [#187](https://github.com/darionxt/mitto/issues/187) adds after the comparison of step 2. A change that a port has to follow goes into the patch run's pull request.
3. Record no Baselines and write no Persona data. Step 2 gives the checkout and the Baselines of the Engine Release the same build, and a patch run is the exception: until the next run, the Engine Release's Persona keeps the build of its Baselines, which `sec-ch-ua-full-version` and `sec-ch-ua-full-version-list` name, while the engine runs the new build. CI's Parity Checks still compare the engine with those Baselines, so a gap there is network code that the patch build changed, and a decision for the owner.
4. Raise the patch in [the version](#10-the-version), with an entry in `CHANGELOG.md` that names each fix the build brings to exported files, with its severity and CVE from the command, and says that the Persona still names the build of its Baselines. Then [the tag](#11-the-tag) and [the publish](#12-the-publish), as in a run.

## What changes for callers

[`CHANGELOG.md`](../CHANGELOG.md) says what changes for callers in each version, under a heading `## X.Y.Z`, newest first: what a Session sends now, what a caller has to change, and what an older mitto cannot read any more. A change that callers will notice writes it there, in the pull request that makes the change. It goes under the version that ships it: the next minor version, whether a run makes it or [a change ships in one of its own](#when-a-run-starts), or a patch that ships before it. The notes of the version's GitHub release hold the entry after their first paragraph, which names the Supported Window and the Engine Release.

Every version needs an entry, a patch too. A patch is made for its fix, and its notes are where a caller reads what it fixes and whether to take it. Without an entry, they would only say again what the notes of the version before said.

The release workflow writes the notes before it builds anything, and a dry run of a pre-release, such as `0.2.0rc1`, takes the entry of the release it comes before, so it shows the notes as that release will have them. GitHub shows each line of the notes on a line of its own, so a paragraph or an item of a list is one line, as on these pages. A relative link, such as `docs/api.md#machine`, leads from the repository's root, and the notes link it at the version's tag.

## Lines that name a Chrome Release

A line in the README or the docs that names a Chrome Release does one of two things:

- **It records what was measured.** It says which build, and usually which day, gave the result, such as the sections of [baselines.md](baselines.md) on each recording, what [capture.md](capture.md) measured, [what differs between Chrome Releases](persona.md#what-differs-between-chrome-releases), the milestones in [roadmap.md](roadmap.md) and what [`CHANGELOG.md`](../CHANGELOG.md) says a version changed. It stays as it was when the Supported Window moves, also once its Chrome Release has left, because it is still what was measured. A new measurement gets a record of its own, which says what it replaces.
- **It states the present Supported Window.** It follows the Window, and only these places hold one:
  - The line of [`README.md`](../README.md) that says which Chrome Releases this version presents, which [step 8](#8-drop-the-oldest-chrome-release) writes. A test checks it against `WINDOWS_PERSONAS` and `ENGINE_RELEASE_PERSONA`, and that no other line of the README names a Chrome Release.
  - [`persona=` in api.md](api.md#persona), which names every Persona and shows one in its example. A test checks that it names every Persona in `WINDOWS_PERSONAS`.
  - [Where each Chrome Release comes from](baselines.md#where-each-chrome-release-comes-from), with the build and the installer of each Chrome Release's Baselines.

Everywhere else, a page says "the Supported Window", "the Engine Release" or "an older Chrome Release", and links to one of those places. A procedure writes `RELEASE` or `BUILD` where a Chrome Release or a build goes.
