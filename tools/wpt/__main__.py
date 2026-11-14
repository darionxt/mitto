"""web-platform-tests in Pages, against Chrome's results: uv run python -m tools.wpt COMMAND. See docs/wpt.md."""

import argparse
import signal
import sys
import tempfile
import time
from collections.abc import Iterable, Sequence
from pathlib import Path

from tools.wpt.check import check, list_differences, run_tests
from tools.wpt.chrome import ChromeError, record
from tools.wpt.copying import copy_tests, write_tests
from tools.wpt.counts import count_by, count_by_owner, owner_table, table
from tools.wpt.differences import (
    DifferencesError,
    dumps_differences,
    flaky_tests,
    loads_differences,
)
from tools.wpt.files import CHROME_FILE, DIFFERENCES_FILE, TESTS_FILE, WPT_ROOT
from tools.wpt.owners import NO_OWNER, move, owner_of
from tools.wpt.results import (
    ChromeResult,
    ResultsError,
    dumps_chrome_results,
    dumps_results,
    loads_chrome_results,
    loads_results,
)
from tools.wpt.server import ServerError, WptServer
from tools.wpt.tests import ListedTest, ListedTestsError, loads_tests

_PROGRESS_EVERY = 250


def main() -> int:
    parser = argparse.ArgumentParser(
        prog="python -m tools.wpt", description="web-platform-tests in Pages, against Chrome's results."
    )
    commands = parser.add_subparsers(dest="command", required=True)

    copy = commands.add_parser("copy", help="copy the tests from the Engine Release's checkout, and list them")
    copy.add_argument("checkout", type=Path, help="the checkout's src directory")

    run = commands.add_parser("run", help="run the tests in Pages, and compare with Chrome's and the list")
    _run_arguments(run)
    run.add_argument("--results", type=Path, help="also write each test's result to this file")

    write = commands.add_parser("differences", help=f"write {DIFFERENCES_FILE.name} again")
    _run_arguments(write)
    write.add_argument(
        "--from",
        dest="sources",
        type=Path,
        nargs="+",
        metavar="RESULTS",
        help="take the results from these files of earlier runs, such as CI's, rather than running the tests",
    )

    record = commands.add_parser(
        "record", help=f"record Chrome's results on the Windows machine, into {CHROME_FILE.name}"
    )
    record.add_argument("--chrome", type=Path, required=True, help="chrome.exe of the Engine Release")
    record.add_argument(
        "--user-data-dirs", type=Path, required=True, help="a new directory, for a user data directory per recording"
    )
    record.add_argument(
        "--windows", type=int, default=8, help="windows, each a Chrome of its own (default: %(default)s)"
    )
    record.add_argument(
        "--times", type=int, default=2, help="recordings, each from a new directory (default: %(default)s)"
    )
    record.add_argument("tests", nargs="*", metavar="PREFIX", help="only the tests whose address starts so")

    commands.add_parser("serve", help="serve the tests on their names until interrupted, to open one by hand")

    commands.add_parser("counts", help="print how many tests and subtests differ, by owner, part and directory")

    owner = commands.add_parser("owner", help="print how many differences a ticket owns, which is none once it closes")
    owner.add_argument("ticket", help=f'such as "#283", or "{NO_OWNER}" for those that no ticket owns')
    owner.add_argument("--list", action="store_true", help="also print each of them")

    give = commands.add_parser("move", help="give a ticket the differences that its missing behaviour causes")
    give.add_argument("ticket", help='such as "#283"')
    give.add_argument("tests", nargs="*", metavar="PREFIX", help="only those of the tests whose address starts so")
    give.add_argument("--outcome", help="only those whose outcome in a Page contains this text")

    args = parser.parse_args()
    # So that a runner that is told to stop still stops its server and its Chrome on the way out.
    signal.signal(signal.SIGTERM, _exit_on_signal)
    try:
        match args.command:
            case "copy":
                return _copy(args.checkout)
            case "run":
                return _run(args)
            case "differences":
                return _write_differences(args)
            case "record":
                return _record(args)
            case "serve":
                return _serve()
            case "owner":
                return _print_owner(args.ticket, args.list)
            case "move":
                return _move(args.ticket, args.tests, args.outcome)
            case _:
                return _print_counts()
    except (ListedTestsError, ResultsError, DifferencesError, ServerError, ChromeError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


def _exit_on_signal(number: int, frame: object) -> None:
    raise SystemExit(128 + number)


def _run_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--jobs", type=int, default=12, help="Pages at once (default: %(default)s)")
    parser.add_argument("tests", nargs="*", metavar="PREFIX", help="only the tests whose address starts so")


def _copy(checkout: Path) -> int:
    tests = copy_tests(checkout, WPT_ROOT)
    write_tests(TESTS_FILE, tests)
    not_run = sum(not test.runs for test in tests)
    print(f"Copied {len(tests)} tests to {WPT_ROOT}, {not_run} of which need testdriver.js and do not run.")
    print(f"Wrote {TESTS_FILE}. Name the build in ORIGIN.md beside it, and record Chrome's results again.")
    return 0


def _selected(prefixes: Sequence[str]) -> list[ListedTest]:
    tests = loads_tests(TESTS_FILE.read_text(encoding="utf-8"))
    return [test for test in tests if test.runs and (not prefixes or test.test.startswith(tuple(prefixes)))]


def _run(args: argparse.Namespace) -> int:
    chrome = loads_chrome_results(CHROME_FILE.read_text(encoding="utf-8"))
    listed = loads_differences(DIFFERENCES_FILE.read_text(encoding="utf-8"))
    outcome = check(_selected(args.tests), chrome, listed, args.jobs)
    if args.results is not None:
        args.results.write_text(dumps_results(outcome.results), encoding="utf-8")
    if outcome.agreed_again:
        print(
            f"{len(outcome.agreed_again)} tests agreed with the list when run again: {', '.join(outcome.agreed_again)}"
        )
    if outcome.changes:
        print(outcome.changes)
    if outcome.unrecorded:
        print(f"No result of Chrome for {len(outcome.unrecorded)} tests: {', '.join(outcome.unrecorded[:20])}")
    changes = outcome.changes
    print(
        f"{len(outcome.found)} differences from Chrome: {len(changes.unlisted)} not in the list, {len(changes.gone)} gone."
    )
    return 0 if outcome else 1


def _write_differences(args: argparse.Namespace) -> int:
    chrome = loads_chrome_results(CHROME_FILE.read_text(encoding="utf-8"))
    listed = loads_differences(DIFFERENCES_FILE.read_text(encoding="utf-8")) if DIFFERENCES_FILE.exists() else []
    tests = _selected(args.tests)
    wanted = {test.test for test in tests}
    if args.sources:
        runs = [
            [result for result in loads_results(source.read_text(encoding="utf-8")) if result.test in wanted]
            for source in args.sources
        ]
    else:
        runs = [run_tests(tests, args.jobs)]
    ran = {result.test for run in runs for result in run}
    written, unrecorded = list_differences(runs, chrome, [d for d in listed if d.test in ran])
    kept = [difference for difference in listed if difference.test not in ran]
    DIFFERENCES_FILE.write_text(dumps_differences([*kept, *written]), encoding="utf-8")
    print(
        f"Wrote {len(kept) + len(written)} differences to {DIFFERENCES_FILE}, {len(flaky_tests(written))} flaky tests."
    )
    if unrecorded:
        print(
            f"No result of Chrome for {len(unrecorded)} tests, which the list leaves out: {', '.join(unrecorded[:20])}"
        )
    return _print_counts()


def _print_counts() -> int:
    tests = loads_tests(TESTS_FILE.read_text(encoding="utf-8"))
    chrome = loads_chrome_results(CHROME_FILE.read_text(encoding="utf-8"))
    listed = loads_differences(DIFFERENCES_FILE.read_text(encoding="utf-8"))
    print("\n".join(owner_table(count_by_owner(chrome, listed))))
    print()
    print("\n".join(table(count_by(tests, chrome, listed, by_part=True), "Part")))
    print()
    print("\n".join(table(count_by(tests, chrome, listed), "Directory")))
    return 0


def _print_owner(ticket: str, each: bool) -> int:
    chrome = loads_chrome_results(CHROME_FILE.read_text(encoding="utf-8"))
    owned = [d for d in loads_differences(DIFFERENCES_FILE.read_text(encoding="utf-8")) if owner_of(d) == ticket]
    if each:
        for difference in owned:
            where = difference.test if difference.subtest is None else f"{difference.test}, {difference.subtest}"
            print(f"{where}: {difference.mitto}")
    count = count_by_owner(chrome, owned).get(ticket)
    if count is None:
        print(f"{ticket} owns no difference.")
    else:
        print(
            f"{ticket} owns {_counted(count.differences, 'difference')} in {_counted(count.tests, 'test')}, "
            f"{_counted(count.differing_subtests, 'subtest')}."
        )
    return 0


def _counted(number: int, thing: str) -> str:
    return f"{number:,} {thing}" if number == 1 else f"{number:,} {thing}s"


def _move(ticket: str, prefixes: Sequence[str], outcome: str | None) -> int:
    listed = loads_differences(DIFFERENCES_FILE.read_text(encoding="utf-8"))
    try:
        written, moved = move(listed, ticket, prefixes, outcome)
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    DIFFERENCES_FILE.write_text(dumps_differences(written), encoding="utf-8")
    print(f"Gave {ticket} {_counted(moved, 'difference')} in {DIFFERENCES_FILE}.")
    return 0


def _record(args: argparse.Namespace) -> int:
    plan = sorted(((test.test, test.timeout) for test in _selected(args.tests)), key=lambda item: -item[1])
    recorded: list[ChromeResult] = []
    for number in range(1, args.times + 1):
        directory: Path = args.user_data_dirs / str(number)
        if directory.exists():
            print(f"error: {directory} exists; give a new directory", file=sys.stderr)
            return 1
        recording = record(args.chrome, directory, plan, args.windows)
        (args.user_data_dirs / f"results-{number}.json").write_text(dumps_results(recording.results), encoding="utf-8")
        if recording.hidden:
            hidden = args.user_data_dirs / f"hidden-{number}.txt"
            hidden.write_text("".join(f"{test}\n" for test in sorted(recording.hidden)), encoding="utf-8")
            print(f"{len(recording.hidden)} tests reported from a hidden page, which {hidden} names.")
        recorded += [ChromeResult.of(result) for result in recording.results]
    if args.tests and CHROME_FILE.exists():
        kept = [
            result
            for results in loads_chrome_results(CHROME_FILE.read_text(encoding="utf-8")).values()
            for result in results
            if not result.test.startswith(tuple(args.tests))
        ]
        recorded = kept + recorded
    CHROME_FILE.write_text(dumps_chrome_results(recorded), encoding="utf-8")
    flaky = _flaky(recorded)
    print(f"Wrote Chrome's results for {len({r.test for r in recorded})} tests to {CHROME_FILE}, {len(flaky)} flaky.")
    return 0


def _flaky(recorded: Iterable[ChromeResult]) -> set[str]:
    seen: dict[str, ChromeResult] = {}
    flaky: set[str] = set()
    for result in recorded:
        if result.test in seen and seen[result.test] != result:
            flaky.add(result.test)
        seen.setdefault(result.test, result)
    return flaky


def _serve() -> int:
    with (
        tempfile.TemporaryDirectory(prefix="mitto-wpt-", ignore_cleanup_errors=True) as results,
        WptServer(Path(results)) as server,
    ):
        print(f"Serving {WPT_ROOT}, such as {server.url_for('dom/nodes/Node-textContent.html')}. Ctrl-C stops.")
        print(
            "A browser needs the names on loopback and the certificate trusted, as `server.settings` gives a Session."
        )
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
