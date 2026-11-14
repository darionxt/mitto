# web-platform-tests

`wpt` is a small root of [web-platform-tests](https://github.com/web-platform-tests/wpt), copied from the Engine Release's tree by `uv run python -m tools.wpt copy`, unchanged. It holds the directories that the Runtime is held to, as [darionxt/mitto#260](https://github.com/darionxt/mitto/issues/260) chose them, whole, `resources`, `common` and `interfaces` whole, every other file that their files name, and the part of the tools that `wpt serve` and the manifest need. Chromium's own files in that tree are left out: its baselines (`*-expected.txt`), `OWNERS` and `DIR_METADATA`, and upstream's `META.yml` and `WEB_FEATURES.yml`, which no test reads. The tests are under the 3-Clause BSD License in `wpt/LICENSE.md`, and the tools' third-party code under the licences in each of their directories.

They are here because CI has no checkout, and the source export holds only what a build reads. [docs/wpt.md](../../docs/wpt.md) says how a run uses them, and each release run copies them again from the new Engine Release's tree ([releasing.md](../../docs/releasing.md#2-check-out-the-new-engine-release)).

- Engine Release: 157.0.8103.39.
- Tests: `third_party/blink/web_tests/external/wpt`, which is upstream's `eed8aea45725da5b002ee8778400d14148d0ebfe`, imported by Chromium on 2026-09-30 with 17 changes of its own, and 238 more Chromium commits to that directory up to the tag. No file of the tree names the upstream commit: the message of Chromium's import commit does ("Import wpt@").
- Tools: `third_party/wpt_tools/wpt/tools`, upstream's `62d90b5ff1e7ac93c0e75bde48fd6ff66dadfda7`, as its `README.chromium` says.

Beside them are what the harness reads: `tests.json`, the copy's testharness.js tests, which the copy tool writes; `chrome.json`, Chrome's results for them, which a recording on the Windows machine writes; and `differences.json`, the list of where a Page's results differ from Chrome's.
