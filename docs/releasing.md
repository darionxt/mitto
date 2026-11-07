# Releasing

Each mitto release ships one engine, built from the **Engine Release**, and presents every **Chrome Release** of the **Supported Window** as **Persona** data on it ([ADR-0005](adr/0005-older-releases-as-data.md)). Chrome ships a new stable milestone every few weeks, and the routine on this page moves mitto with it. It lists every step in order, with the machine it runs on and whether it needs the owner's OK. Terms in **bold** are defined in [CONTEXT.md](../CONTEXT.md).

## When a run starts

A run starts when a milestone reaches its stable date in [Chromium Dash](https://chromiumdash.appspot.com/schedule), not its early stable date. About two weeks before the stable date, the early stable date gives the milestone to a small share of users only, and most installations are still on the one before. The run makes that milestone the Engine Release, records every Chrome Release of the new Supported Window, drops the oldest, and publishes.

A run raises the minor version: 0.1.0, 0.2.0 and so on. A fix between two runs raises the patch, and goes through the [last three steps](#9-the-version) only. Dropping a Chrome Release, and with it its Persona, only happens in a run.

## The steps

| Step | Where | Who approves |
| --- | --- | --- |
| 1. [Find the builds](#1-find-the-builds) | Any machine | Nobody |
| 2. [Check out the new Engine Release](#2-check-out-the-new-engine-release) | The workspace's macOS and Linux machines | Nobody |
| 3. [Make the source export](#3-make-the-source-export) | The workspace's macOS and Linux machines | The owner, before it is published |
| 4. [Record Baselines](#4-record-baselines), with the Platform Runtime component | A Windows machine | Nobody |
| 5. [Check for Merkle Tree Certificates](#5-check-for-merkle-tree-certificates) | The workspace, the Windows machine and any machine | The owner, if a seed turns them on |
| 6. [Write the Persona data](#6-write-the-persona-data) | Any machine | Nobody |
| 7. [Drop the oldest Chrome Release](#7-drop-the-oldest-chrome-release) | Any machine | Nobody |
| 8. [The known-gaps list](#8-the-known-gaps-list) | A macOS or Linux **Host** | The owner, for any gap that stays |
| 9. [The version](#9-the-version) | Any machine | Review of the run's pull request |
| 10. [The tag](#10-the-tag) | Any machine | The owner |
| 11. [The publish](#11-the-publish) | GitHub's hosted runners | The owner, in the `pypi` environment |

"Any machine" is one with the repository and uv. The scripts of steps 1, 6 and 7 need no native library. Steps 2 to 9 end in one pull request, and CI builds it from the new export on both Hosts.

### 1. Find the builds

```bash
uv run python -m tools.release builds
```

This prints the next Supported Window: the milestone after the newest Persona, as the Engine Release, and the two before it. From Chromium Dash it takes the Engine Release's stable date and early stable date. From [Google's version history](https://versionhistory.googleapis.com/v1/chrome/platforms/win64/channels/stable/versions/all/releases?filter=endtime=none) it takes the newest Windows stable build of each Chrome Release that can be pinned, and prints it beside the build of the Chrome Release's Baselines. Windows is used because every Persona is Windows Chrome. `--engine RELEASE` names another Engine Release, such as during a run, once its Persona exists.

Each Chrome Release is recorded at its newest pinnable build, because that is the one Google's update server gives for it ([Install Chrome](baselines.md#install-chrome)). A newer build in a partial rollout cannot be pinned, and is not recorded. Each run records the older Chrome Releases again at their newest pinnable builds, because `sec-ch-ua-full-version-list` names the build. An Engine Release with no pinnable build yet has `none yet`, and the run waits for one.

### 2. Check out the new Engine Release

1. Write the new Engine Release's newest pinnable build from [step 1](#1-find-the-builds) into [`tools/workspace/chromium_tag`](../tools/workspace/chromium_tag). Its checkout and its Baselines take the same build, so that the engine runs the network code that was recorded.
2. Rerun [`tools/workspace/bootstrap`](building.md#set-up-the-workspace) on each machine. It creates `$MITTO_WORKSPACE/chromium-<tag>` for the new release, with its own checkout and `out/mitto`. If a patch no longer applies, the script stops after the sync. Update the patch against the new checkout and rerun the script. Delete a patch the new release no longer needs.
3. [Build the native core](building.md#build-the-native-core) in the new checkout.
4. [Compare](building.md#compare-two-releases) the build of each older Chrome Release in the new Supported Window with the new one. A change in the network code that sets up a connection is one that Persona data may have to undo for the older Chrome Releases ([what differs between Chrome Releases](persona.md#what-differs-between-chrome-releases)). It is a decision for the milestone's plan.
5. Once the new release works, delete the old release's `chromium-<tag>` directory.

Each release has its own checkout because a shallow checkout cannot move to a new release in place: gclient would fetch every branch with full history. Until the last step, the old checkout and its build output stay usable.

### 3. Make the source export

[Make the source export](building.md#make-the-source-export) of the new release, publish it with the owner's OK, and write its URL and SHA-256 into [`tools/workspace/source_export.json`](../tools/workspace/source_export.json). CI then builds the native core from it.

On each of the two machines, [update its part of the third-party notices](building.md#third-party-notices) from a build of the new export, such as the one that checks the archive, and commit the result with the run's pull request. CI's test of the installed wheel fails on a Host whose part is wrong.

From the same build on either machine, check [ICU's data](building.md#icus-data) again. Disassemble the unstripped library with `objdump -d`, and find every call to ICU's entry points to its data, whose names end in the ICU version, such as `ures_openDirect_78`. A caller that the table of that section does not name, or a new caller of one that passes a name on, such as `base::CodepageToUTF16()`, may ask for an item that the library lacks. Read what it asks for, and add the item to [`native/make_icu_data.py`](../native/make_icu_data.py). The build stops if an item in that list is missing from ICU's new data.

### 4. Record Baselines

On the Windows machine, [record Baselines](baselines.md) of every Chrome Release in the new Supported Window: the Engine Release [installed](baselines.md#install-chrome), and the older ones [unpacked](baselines.md#an-older-chrome-release), each at the build that the update server gives. [Check and commit](baselines.md#check-and-commit) them, update [where each Chrome Release comes from](baselines.md#where-each-chrome-release-comes-from), and write down what the recording found under its Chrome Release on that page.

Then check for the Platform Runtime component, which [no Chrome has shown so far](capture.md#what-this-does-not-cover-1). Start Chrome of each Chrome Release from a new user data directory with [the first five flags](baselines.md#what-it-does), leave it for 15 minutes, and open `chrome://components`. Write down whether it lists Platform Runtime, and whether the user data directory has a `PlatformRuntime` directory. If one does, the component reaches Chrome, and by Chrome's source its library can change the lines Chrome adds for [Google's domains](capture.md#what-chrome-adds-for-googles-domains). That is a decision for the milestone's plan.

### 5. Check for Merkle Tree Certificates

With `VerifyMTCs` on, Chrome adds the IDs of its Merkle Tree CAs to `trust_anchors` ([what this does not cover](capture.md#what-this-does-not-cover)). [`native/feature_list.cc`](../native/feature_list.cc) states it off, so a Session would not follow. When the Engine Release moves, check three places:

- In the new checkout on the workspace, the default of `kVerifyMTCs` in `net/base/features.cc`.
- On the Windows machine, the variations seed of each recording. `chrome://version/?show-variations-cmd` of a recording start must not name `VerifyMTCs`, and neither may the seed itself, `VariationsSafeSeedV2` in the user data directory, once decompressed ([how to read it](baselines.md#the-variations-seed-and-the-country)). `record.ps1` keeps a feature or field trial in `chrome.txt` when its name has `mtc` in it, so `chrome.txt` shows one that a recording drew.
- In the new Baselines, `trust_anchors` in each ClientHello. The Parity Checks of [step 8](#8-the-known-gaps-list) show a gap there if Chrome sent more IDs than the compiled-in root store has.

Write the answer down under the recording in [baselines.md](baselines.md). If any of the three shows `VerifyMTCs` on, what a Session sends is a decision for the milestone's plan.

### 6. Write the Persona data

For each Chrome Release whose Baselines are new:

```bash
uv run python -m tools.release persona RELEASE
```

This writes `src/mitto/personas/chrome_RELEASE_windows.json` from `tests/baselines/RELEASE`, [as persona.md describes](persona.md#write-one). For the new Engine Release, also name its Persona in [`src/mitto/_persona.py`](../src/mitto/_persona.py): a constant like the others, its entry in `__all__` and in `WINDOWS_PERSONAS`, and `ENGINE_RELEASE_PERSONA`. The tests that check what a Session presents by default name the Engine Release, and fail until they move with it. The README's line follows in [step 7](#7-drop-the-oldest-chrome-release).

Then run `uv run pytest tests/test_persona.py tests/release`. If the script refuses, for example because a Content-Type is not the one it knows, Chrome has changed what a request sends. Change the script and [persona.md](persona.md) to say what it does now.

### 7. Drop the oldest Chrome Release

```bash
uv run python -m tools.release drop
```

This deletes the oldest Persona's file, its Baselines and its entries in the known-gaps list, and takes its name out of `src/mitto/_persona.py`. It also writes the line of [`README.md`](../README.md) that names the Supported Window, from the Personas that are left and `ENGINE_RELEASE_PERSONA`, in the words that `tests/test_docs.py` checks. It refuses while there are only three Personas, so it only runs once the new Engine Release has one. It also refuses, without changing anything, if `_persona.py` names the oldest anywhere else, if the Engine Release is not the newest Persona, or if the README has no such line. It then prints any other line of code that still names it, by its Persona, a build or its `User-Agent`. A line in `tests/release` that holds what Chromium Dash said on one day is a record and stays.

Then go through the docs with the [rule below](#lines-that-name-a-chrome-release), with the dropped Chrome Release in place of `RELEASE`:

```bash
git grep -n -w RELEASE -- docs
```

### 8. The known-gaps list

On a macOS or Linux Host, [build the native core from the new export](building.md#build-from-the-source-export), then [write the known-gaps list again](parity.md#the-known-gaps-list) with `MITTO_NATIVE_LIBRARY` set, and read its diff. The Parity Checks now run every Persona on the new engine, so a gap of an older Chrome Release that the run brought is network code that moved. It goes back into Persona data, as [persona.md](persona.md#what-differs-between-chrome-releases) says how, or the owner decides to keep it. Write down what differs between the Chrome Releases of the new Supported Window on that page, as a new record.

### 9. The version

Raise `version` in [`pyproject.toml`](../pyproject.toml) and run `uv lock`, which writes it into `uv.lock` too. The run's pull request then holds everything from step 2 on.

### 10. The tag

Once the pull request is merged, tag its commit on `main` as `vX.Y.Z`, with the version of step 9, and push the tag, with the owner's OK. The tag starts the publish.

### 11. The publish

The tag starts [the release workflow](building.md#the-release-workflow), `.github/workflows/release.yml`. It checks that the tag is the version in `pyproject.toml` and that its commit is on `main`, builds the wheels from nothing, with no Actions cache, on the hosted runners that CI uses, and runs every test against each installed wheel. It then waits for the owner's approval in the `pypi` environment: approve the run's deployment once both builds have passed. It uploads the wheels to PyPI with Trusted Publishing. No API token can upload to `mitto`. The symbols archives go to the GitHub release of the tag, not to PyPI. There are wheels only, and no sdist. Last, it installs the version from PyPI on each Host and makes a request with it.

A [dry run](building.md#a-dry-run) does the same with a pre-release on TestPyPI, and needs no tag and no approval.

## Lines that name a Chrome Release

A line in the README or the docs that names a Chrome Release does one of two things:

- **It records what was measured.** It says which build, and usually which day, gave the result, such as the sections of [baselines.md](baselines.md) on each recording, what [capture.md](capture.md) measured, [what differs between Chrome Releases](persona.md#what-differs-between-chrome-releases) and the milestones in [roadmap.md](roadmap.md). It stays as it was when the Supported Window moves, also once its Chrome Release has left, because it is still what was measured. A new measurement gets a record of its own, which says what it replaces.
- **It states the present Supported Window.** It follows the Window, and only these places hold one:
  - The line of [`README.md`](../README.md) that says which Chrome Releases this version presents, which [step 7](#7-drop-the-oldest-chrome-release) writes. A test checks it against `WINDOWS_PERSONAS` and `ENGINE_RELEASE_PERSONA`, and that no other line of the README names a Chrome Release.
  - [`persona=` in api.md](api.md#persona), which names every Persona and shows one in its example. A test checks that it names every Persona in `WINDOWS_PERSONAS`.
  - [Where each Chrome Release comes from](baselines.md#where-each-chrome-release-comes-from), with the build and the installer of each Chrome Release's Baselines.

Everywhere else, a page says "the Supported Window", "the Engine Release" or "an older Chrome Release", and links to one of those places. A procedure writes `RELEASE` or `BUILD` where a Chrome Release or a build goes.
