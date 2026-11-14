# web-platform-tests

M8 holds the **Runtime** to Chrome's results in [web-platform-tests](https://web-platform-tests.org/), failures included ([roadmap.md](roadmap.md)). A harness runs each testharness.js test of the chosen directories in a **Page**, compares each subtest's result with what Windows Chrome of the **Engine Release** gave, and holds the differences to a list in the repository, as the [Parity Checks](parity.md) hold theirs to their **Known Gaps**. Terms in **bold** are defined in [CONTEXT.md](../CONTEXT.md). [darionxt/mitto#260](https://github.com/darionxt/mitto/issues/260) found how, and chose the directories.

## The files

Everything is in [`tests/wpt`](../tests/wpt):

| File | What it holds | What writes it |
| --- | --- | --- |
| `wpt/` | A small root of web-platform-tests, copied from the Engine Release's tree, with the tools that serve it. [`ORIGIN.md`](../tests/wpt/ORIGIN.md) says from where | `uv run python -m tools.wpt copy`, in a release run |
| `tests.json` | Each testharness.js test of the copy, one per line: its address, whether it has the long timeout, and what it needs that a Page cannot give in M8 | The same command |
| `chrome.json` | Chrome's result for each test that runs, one per line: the harness status, how many subtests ran, and each subtest that does not pass, with its status and message. A flaky test has a line for each of its two results | `uv run python -m tools.wpt record`, on the Windows machine |
| `differences.json` | Each place where a Page's result differs from Chrome's, one per line | `uv run python -m tools.wpt differences` |

The code is in [`tools/wpt`](../tools/wpt).

## Which tests

The copy holds the directories of #260's table, which cover each part of M8, and the rest of that ticket's list, whole: [`tools/wpt/parts.py`](../tools/wpt/parts.py) names them, and which ticket each part is. A test is a page with its query, as wptserve serves it, such as `encoding/textdecoder-labels.any.html` and its worker variant `encoding/textdecoder-labels.any.worker.html`, both made from one `.any.js`. A test runs in a window, and an `.any.js` or `.worker.js` test also in a dedicated worker. Its variants for shared workers, service workers and ShadowRealm are not in the list, since M8 leaves those scopes out. Reftests, manual tests and crashtests are out too.

Of the 8,672 tests, the 469 that drive the browser through `testdriver.js` do not run: they need input or other automation, which no recording of Chrome here gives either, and they stay listed in `tests.json` with `testdriver` in what they need. 8,203 tests run.

What else a test needs that a Page cannot give in M8 comes from #260's scan of the test and the scripts it includes: layout or style, a second window, a service or shared worker, WebSocket, graphics, media or WebCrypto. The scan reads text, so it finds a need a little too often. Such a test runs like any other, and its differences stay in the list until a later milestone gives what it needs.

## Run them

A run needs a native library with the Page program beside it, as a wheel holds them, or as `out/mitto` and `out/runtime` of a build give them once both files are in one directory ([building.md](building.md#build-the-runtime)), and the ports below free on loopback:

```bash
uv run python -m tools.wpt run
```

It prints each difference that the list does not have, and each listed one that is gone, with how many there are, and exits with 1 if there are any. A prefix runs only the tests whose address starts with it, such as `uv run python -m tools.wpt run dom/nodes/`, and holds only their part of the list. `--jobs N` runs N Pages at once, 12 by default, since most of them only wait out a timeout, and `--results FILE` also writes each test's result to a file. `uv run python -m tools.wpt serve` serves the tests until you stop it, to open one by hand.

## What a run does

1. It starts `wpt serve` from the copy's tools, in a process of its own, on Chromium's ports for web-platform-tests: HTTP on 8001 and 8081, HTTPS on 8444 and 8445, HTTP/2 on 9000, and the others of `PORTS` in [`server.py`](../tools/wpt/server.py). Messages that name an origin, such as `http://web-platform.test:8001`, are then those of Chrome's runs. It serves `web-platform.test`, `not-web-platform.test` and their `www`, `www1`, `www2` and IDN names, with the copy's certificate, which names each of them.
2. It serves mitto's own [`testharnessreport.js`](../tools/wpt/reporter/testharnessreport.js) in place of the browser's, which asks testharness.js to wait for the runner rather than time out by itself, notes the address that the test started at, before the test can push state or change its fragment, and once the tests have completed posts every subtest's name, status and message and the harness status to [`collect.py`](../tools/wpt/reporter/collect.py), a handler of the server that keeps them. Nothing reaches into the Page from outside while a test runs, as with the probe of the Script Fingerprint.
3. For each test, it opens a new Page on a new **Session** at the test's URL, as a caller's Page loads a page: HTTPS for a name with `.https.`, HTTP/2 for `.h2.` and the `www` name for `.www.`, as wptrunner has it. The Session presents the Engine Release's **Persona** and one **Machine**, drawn from a fixed seed, so that what a test reads of it, such as the window's height in a message, is the same in every run and on every Host. It finds the server's names on loopback and accepts its certificate through settings that only the private `_settings` argument reaches, as the Parity Checks find the capture server.
4. A test has wptrunner's timeout, 10 seconds, or 60 for one whose manifest entry says `long`, from the start of its Page. A test that has posted nothing by then gives its results through `evaluate()`, for a page whose Content Security Policy refused the post, or ran out of time. The Page ending first is a crash.
5. It compares each test's result with Chrome's by [Blink's rule](#blinks-rule), and the differences with the list.

A busy machine can make a test late once, so each test that disagrees with the list runs again, and its second result counts if that one agrees. The run names such tests.

A test that runs out of time in a Page as the list has it, because the list says so or because it runs out of time in Chrome and has no line, gets 3 seconds rather than its full timeout, so that a run stays short while many tests wait for what the Runtime lacks. One that finishes within them shows as a difference. One that would finish later stays as listed until `differences` writes the list from a run of its own, which gives every test its full timeout.

## Blink's rule

Blink compares a run with a baseline in `wpt_results_processor.py`, and so does the harness:

- The harness status must be Chrome's, with its message for an error.
- Each subtest's status must be Chrome's, which is PASS for one that Chrome's result does not list.
- A FAIL or PRECONDITION_FAILED must have Chrome's message, where Chrome's has one. The message of a TIMEOUT or NOTRUN does not count.
- Every subtest that Chrome's result lists must have run. Chrome's result does not name the subtests that pass, so a different number of subtests is one more difference.
- A test that ran out of time, or whose browser ended, on either side compares only that, as Blink records a test that runs out of time as `Timeout` alone: how far it got depends on when it stopped.

A flaky test, whose two recordings of Chrome differ, matches either: a subtest differs only where it differs from both.

Before they compare, messages have what differs from run to run replaced by a placeholder: each UUID by `<uuid>`, such as a token that a handler keys a stash with, and each number with eight decimals or more, or thirteen digits or more, by `<number>`, such as one from `Math.random()`, `performance.now()` or `Date.now()`. Chrome's results and the list hold them so.

## The list of differences

`tests/wpt/differences.json` holds each difference, one per line, with the keys `test`, `subtest`, `chrome` and `mitto`. `subtest` is `null` for the test as a whole: its harness status, or how many subtests ran, such as `"222 subtests"`. `chrome` and `mitto` are each side's outcome, such as `"PASS"`, `"FAIL: assert_true: expected true got false"` or `"TIMEOUT"`, and `null` for a subtest that one side did not run. A test with no line matches Chrome, and `grep '"test": "dom/nodes/' tests/wpt/differences.json` lists a directory's.

A test whose result in a Page varies between runs or Hosts, such as one that depends on timing, has one line whose `mitto` is `"FLAKY"`, with `subtest` and `chrome` `null`, and none of its differences: a run does not compare it.

A run fails on a difference that is not in the list and on one in the list that is gone. A difference whose outcome has changed counts as both, so the list always says exactly how a Page differs from Chrome. There is one list for every **Host**. Each of M8's tickets empties its part of it, and M8's exit leaves only the differences of tests that need what a later milestone gives.

To write the list again from what the Page does now, with every test at its full timeout:

```bash
uv run python -m tools.wpt differences
```

It takes about 80 minutes on the Mac with its 12 Pages at once, and prints how many tests and subtests differ in each part of M8 and each directory. `--from FILE ...` writes it from the results of earlier runs instead, such as those that CI's job keeps on each Host, and marks a test flaky where they disagree. Those runs gave 3 seconds to each test that the list had run out of time, so such a test stays out of time in the list even if it now finishes later than that. A test that the list marks stays marked: delete its line by hand once it no longer varies. A prefix writes only the part of those tests. `uv run python -m tools.wpt counts` prints the counts of the list as it is. Read the diff before you commit it: fixing something only removes lines, and an added line means the Page moved away from Chrome.

A pull request that changes what a Page does changes the list in the same pull request. Its `wpt` job fails until then, and keeps the results that write the list for every Host at once, from the run's number, into a directory to delete afterwards:

```bash
gh run download RUN --pattern 'wpt-results-*' --dir wpt-results
uv run python -m tools.wpt differences --from wpt-results/*/wpt-results.json
```

How many subtests differ counts each subtest whose outcome differs, each that one side ran and the other did not, and all of Chrome's subtests of a test that ran out of time or crashed on one side only.

## Chrome's results

They are Windows Chrome's of the Engine Release, as a Baseline is ([baselines.md](baselines.md)), recorded again in each release run. #260 found that none of the results in the Engine Release's tree is Chrome's as shipped: they are mostly `headless_shell`'s, with Blink's test and experimental features on, and differ from stable Chrome for about 4% of the tests.

On the Windows machine, from the repository's root, with Chrome of the Engine Release [installed](baselines.md#install-chrome) and a new directory:

```powershell
uv run python -m tools.wpt record --chrome $env:LOCALAPPDATA\Google\Chrome\Application\chrome.exe --user-data-dirs $env:USERPROFILE\mitto-wpt\a
```

Chrome runs windowed on the desktop, never headless, as for Baselines. Over SSH, start it through a task with an Interactive principal, as `record.ps1` starts Chrome, so that its windows are on the desktop and a dropped connection does not end it, and read its log:

```powershell
$record = "cd /d $env:USERPROFILE\mitto && uv run python -m tools.wpt record --chrome $env:LOCALAPPDATA\Google\Chrome\Application\chrome.exe --user-data-dirs $env:USERPROFILE\mitto-wpt\a > wpt-record.log 2>&1"
$action = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument "/c $record"
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName mitto-wpt -Action $action -Principal $principal | Out-Null
Start-ScheduledTask -TaskName mitto-wpt
Unregister-ScheduledTask -TaskName mitto-wpt -Confirm:$false
Get-Content "$env:USERPROFILE\mitto\wpt-record.log"
```

For each of two recordings, it:

1. starts Chrome from a new user data directory, `N/seed` under the given one, with the first five flags of [a recording session](baselines.md#what-it-does), waits until `Local State` has the variations seed, and closes it;
2. copies that directory once for each window, 8 by default (`--windows`), and starts a Chrome from each copy with those flags, the server's certificate and names, `--no-proxy-server` and a debugging port of its own from 9207 up, with the server as for a run in a Page. So tests in different windows share no cookies or storage, as each test in a Page has a Session of its own, and each Chrome runs its tests one after another, as wptrunner runs a browser;
3. opens each test in a new window of its Chrome, as wptrunner gives each test a window, with the test's timeout. The windows are tiled so that none covers another, and each Chrome's first window is minimized, since Windows opens a window of a process in the background behind the windows in front: Chrome hides a window that something covers, and a hidden page runs its timers late and no animation frames. The reporter notes the page's visibility. A test that reported from a hidden page runs again, twice at most, and `hidden-N.txt` names any that never reported from a visible one;
4. keeps each result in `results-N.json` beside the user data directories, and closes each Chrome.

It then writes `chrome.json` from both recordings: a test whose two results differ is flaky, and keeps both. A prefix records only those tests and keeps the others' lines as they are. It prints Chrome's build, which must be the Engine Release's, as `tools/workspace/chromium_tag` names it. The two recordings take about 70 minutes. Copy `chrome.json` back to the repository, and the results files if you want to read them, which name no path or machine.

Chrome as shipped blocks a window that a page opens without a gesture, so a test that opens one fails or runs out of time in Chrome's results, as it does in a real Chrome that nobody clicks in. wptrunner passes `--disable-popup-blocking`, and Blink's results differ there.

## In CI

The `wpt` job of CI runs on every Host once the native job has built the wheels, from the installed wheel, as the native job tests it: `python -m tools.wpt run --jobs 12`, since most Pages wait out a timeout with nothing to do. It keeps each test's result as the artifact `wpt-results-RUNNER`, whose files `--from` takes. A draft pull request runs no native job, so no `wpt` job either. On each Host it takes about 20 minutes.
