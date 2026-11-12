"""Tests of a Page from Python: Session._open_page() and AsyncSession._open_page(), which stay private until M10.

Each test that opens a Page goes through both APIs, and needs a native library with the Page program beside it, as CI's
native job installs it on every Host. The rest need nothing.
"""

import concurrent.futures
import json
import os
import shutil
import signal
import subprocess
import sys
import textwrap
import threading
import time
from collections.abc import Iterator
from pathlib import Path
from typing import Any, cast

import pytest

import mitto
from mitto import _native, _page
from mitto._errors import PageEndedError, PageError, PageTimeoutError, ScriptError
from mitto._native import ConsoleMessage, PageEnded, PageErrorCode, PageEvaluated, UncaughtError
from mitto._page import KEPT_REPORTS, Page, PageRecord, outcome_of
from mitto._timeout import to_seconds
from tests.page_processes import process_is_gone, wait_until_gone

# A Page that navigates nowhere, so that these tests need no server. tests/test_page_loading.py loads documents.
_URL = "about:blank"
# Long enough for a Page process to answer on a busy runner, and short enough to keep the tests quick.
_SHORT_TIMEOUT = 0.3
# Long enough for a timer or a failing request on a busy runner.
_TIMEOUT = 10
_MAX_SCRIPT_SIZE = 16 * 1024 * 1024 - 8


@pytest.fixture
def page_program(native_library: Any, both_apis: str) -> None:
    """Run a test through each API in turn, or skip it if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


@pytest.fixture
def session(page_program: None) -> Iterator[mitto.Session]:
    """Return a Session through each API in turn, or skip if no Page program lies beside the library."""
    with mitto.Session() as session:
        yield session


def _open_page(session: mitto.Session, url: str = _URL) -> Page:
    return session._open_page(url)  # pyright: ignore[reportPrivateUsage]


@pytest.fixture
def page(session: mitto.Session) -> Iterator[Page]:
    with _open_page(session) as page:
        yield page


@pytest.mark.parametrize(
    ("script", "result"),
    [
        pytest.param("1 + 1", 2, id="number"),
        pytest.param("'b\\u00fccher'", "bücher", id="string"),
        pytest.param(
            "({a: [1, 'two', null, true], b: {c: 1.5}})", {"a": [1, "two", None, True], "b": {"c": 1.5}}, id="object"
        ),
        pytest.param("null", None, id="null"),
        pytest.param("undefined", None, id="undefined"),
        pytest.param("(function () {})", None, id="function"),
        pytest.param("NaN", None, id="nan"),
        pytest.param("({toJSON() { return 'mine'; }})", "mine", id="to-json"),
        pytest.param("var x = 1", None, id="statement"),
    ],
)
def test_a_page_gives_a_result_as_json_gives_it(page: Page, script: str, result: Any) -> None:
    assert page.evaluate(script) == result


# What a page reads of its time zone and locale: the zone's name, the offset from UTC at the epoch, the name that Date gives
# the zone there, Intl's locale and a number written in it.
_TIME_ZONE_AND_LOCALE = """
    [
      Intl.DateTimeFormat().resolvedOptions().timeZone,
      new Date(0).getTimezoneOffset(),
      new Date(0).toString().replace(/^.*\\(/, "("),
      Intl.DateTimeFormat().resolvedOptions().locale,
      (1234.5).toLocaleString(),
    ]
"""


@pytest.mark.parametrize(
    ("time_zone", "expected"),
    [
        (None, ["America/New_York", 300, "(Eastern Standard Time)", "en-US", "1,234.5"]),
        ("Asia/Tokyo", ["Asia/Tokyo", -540, "(Japan Standard Time)", "en-US", "1,234.5"]),
        ("UTC", ["UTC", 0, "(Coordinated Universal Time)", "en-US", "1,234.5"]),
    ],
    ids=str,
)
def test_a_page_reports_the_time_zone_of_its_machine_and_the_locale_of_its_persona(
    page_program: None, time_zone: str | None, expected: list[Any]
) -> None:
    machine = mitto.Machine.draw(seed=1) if time_zone is None else mitto.Machine.draw(seed=1, time_zone=time_zone)
    with mitto.Session(machine=machine) as session, _open_page(session) as page:
        assert page.evaluate(_TIME_ZONE_AND_LOCALE) == expected


def test_scripts_share_one_context(page: Page) -> None:
    page.evaluate("var total = 6; function times(n) { return total * n; }")

    assert page.evaluate("times(7)") == 42


def test_a_page_at_about_blank_keeps_that_url_and_has_a_process_of_its_own(page: Page) -> None:
    assert page.url == _URL
    assert page.process_id > 0
    assert page.process_id != os.getpid()
    assert not process_is_gone(page.process_id)


def test_a_page_at_about_blank_has_its_document_timers_and_a_fetch_that_fails(page: Page) -> None:
    # A Page's window is its global object, with about:blank's document when it navigates nowhere. With no document from
    # its Session, it has no Initiator for a request, so fetch() fails as a network error does.
    script = "[window === globalThis, document.documentElement.outerHTML, typeof setTimeout, typeof fetch]"

    assert page.evaluate(script) == [True, "<html><head></head><body></body></html>", "function", "function"]
    assert page.evaluate("new Promise(resolve => setTimeout(resolve, 1, 'later'))", timeout=_TIMEOUT) == "later"
    with pytest.raises(ScriptError, match=r"^Uncaught \(in promise\) TypeError: Failed to fetch$"):
        page.evaluate("fetch('http://127.0.0.1:1/')", timeout=_TIMEOUT)


@pytest.mark.parametrize(
    ("script", "result"),
    [
        pytest.param("Promise.resolve(41).then(n => n + 1)", 42, id="fulfilled"),
        pytest.param("(async () => { await null; await null; return ['later']; })()", ["later"], id="async-function"),
        pytest.param("Promise.resolve()", None, id="undefined"),
    ],
)
def test_evaluate_waits_for_a_promise_to_settle(page: Page, script: str, result: Any) -> None:
    assert page.evaluate(script) == result


def test_evaluate_waits_for_a_promise_that_another_call_settles(page: Page) -> None:
    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
        waiting = executor.submit(page.evaluate, "new Promise(resolve => globalThis.settle = resolve)")
        deadline = time.monotonic() + 10
        while page.evaluate("typeof settle") != "function":
            assert time.monotonic() < deadline
            time.sleep(0.01)
        assert not waiting.done()

        assert page.evaluate("settle({done: true}); 'settled'") == "settled"
        assert waiting.result(timeout=10) == {"done": True}


def test_a_promise_that_nothing_settles_times_out(page: Page) -> None:
    with pytest.raises(PageTimeoutError):
        page.evaluate("new Promise(resolve => {})", timeout=_SHORT_TIMEOUT)


def test_a_promise_that_a_timer_settles_gives_its_result_while_the_page_runs_other_calls(page: Page) -> None:
    page.evaluate("window.later = new Promise(resolve => setTimeout(resolve, 50, 'from a timer'))")

    assert page.evaluate("1 + 1") == 2
    assert page.evaluate("later", timeout=_TIMEOUT) == "from a timer"


@pytest.mark.parametrize(
    ("script", "message"),
    [
        pytest.param("throw new TypeError('no ' + 'good')", "Uncaught TypeError: no good", id="throws"),
        pytest.param("null.x", "Uncaught TypeError: Cannot read properties of null (reading 'x')", id="null"),
        pytest.param("let = ;", "Uncaught SyntaxError: ", id="does-not-compile"),
        pytest.param("throw 'just text'", "Uncaught just text", id="throws-text"),
        pytest.param("Promise.reject(new RangeError('no'))", "Uncaught (in promise) RangeError: no", id="rejected"),
        pytest.param("(async () => { throw new Error('later'); })()", "Uncaught (in promise) Error: later", id="async"),
        pytest.param("10n", "Uncaught TypeError: Do not know how to serialize a BigInt", id="bigint"),
        pytest.param(
            "const a = []; a.push(a); a", "Uncaught TypeError: Converting circular structure to JSON", id="cycle"
        ),
    ],
)
def test_a_script_that_fails_raises_a_script_error_and_the_page_goes_on(page: Page, script: str, message: str) -> None:
    with pytest.raises(ScriptError) as raised:
        page.evaluate(script)

    assert raised.value.message.startswith(message)
    assert str(raised.value) == raised.value.message
    assert page.evaluate("2 + 2") == 4


def test_a_timeout_raises_a_page_timeout_error_and_the_page_goes_on(page: Page) -> None:
    start = time.perf_counter()
    with pytest.raises(PageTimeoutError) as raised:
        page.evaluate("new Promise(resolve => globalThis.late = resolve)", timeout=_SHORT_TIMEOUT)

    assert time.perf_counter() - start >= _SHORT_TIMEOUT * 0.9
    assert isinstance(raised.value, TimeoutError)
    assert raised.value.timeout == _SHORT_TIMEOUT
    assert str(raised.value) == f"the result of the script did not arrive within {_SHORT_TIMEOUT} seconds"
    # Its result, which nothing waits for, goes when it comes.
    assert page.evaluate("late('too late'); 1 + 1") == 2
    assert page.evaluate("3") == 3


def test_a_script_that_never_ends_keeps_the_page_busy_until_it_closes(session: mitto.Session) -> None:
    page = _open_page(session)
    with pytest.raises(PageTimeoutError):
        page.evaluate("while (true) {}", timeout=_SHORT_TIMEOUT)
    with pytest.raises(PageTimeoutError):
        page.evaluate("1", timeout=_SHORT_TIMEOUT)

    page.close()

    assert process_is_gone(page.process_id)


def test_a_page_whose_process_is_killed_raises_page_ended_error_for_each_call(page: Page) -> None:
    assert page.evaluate("1") == 1

    os.kill(page.process_id, signal.SIGTERM if sys.platform == "win32" else signal.SIGKILL)

    with pytest.raises(PageEndedError) as raised:
        page.evaluate("2")
    assert raised.value.message.startswith("the Page process ")
    with pytest.raises(PageEndedError) as again:
        page.evaluate("3")
    assert again.value.message == raised.value.message


def test_closing_a_page_ends_its_process_and_refuses_later_calls(session: mitto.Session) -> None:
    with _open_page(session) as page:
        assert page.evaluate("1") == 1

    assert process_is_gone(page.process_id)
    with pytest.raises(ValueError, match="this Page has been closed"):
        page.evaluate("1")
    page.close()


def test_closing_a_page_ends_a_call_that_waits(session: mitto.Session) -> None:
    page = _open_page(session)
    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
        waiting = executor.submit(page.evaluate, "new Promise(resolve => {})")
        time.sleep(_SHORT_TIMEOUT)
        assert not waiting.done()

        page.close()

        with pytest.raises(ValueError, match="this Page has been closed"):
            waiting.result(timeout=10)
    wait_until_gone(page.process_id)


def test_closing_the_session_ends_its_pages(session: mitto.Session) -> None:
    page = _open_page(session)
    assert page.evaluate("1") == 1

    session.close()

    with pytest.raises(PageEndedError, match="the Session was destroyed"):
        page.evaluate("2")
    wait_until_gone(page.process_id)
    page.close()


def test_a_closed_session_opens_no_page(session: mitto.Session) -> None:
    session.close()

    with pytest.raises(ValueError, match="has been closed"):
        _open_page(session)


def test_several_threads_can_evaluate_on_one_page_at_once(page: Page) -> None:
    def evaluate_many(number: int) -> list[Any]:
        return [page.evaluate(f"[{number}, {each}]") for each in range(20)]

    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
        results = list(executor.map(evaluate_many, range(8)))

    assert results == [[[number, each] for each in range(20)] for number in range(8)]


def test_a_page_keeps_what_its_scripts_write_to_the_console(page: Page) -> None:
    page.evaluate("""
        console.log('a', 1, [2, 3], {b: 4}); console.debug('d'); console.info('i'); console.warn('w');
        console.error('e', '\\u00e9');
    """)

    assert page.console_messages == [
        ConsoleMessage("log", "a 1 2,3 [object Object]"),
        ConsoleMessage("debug", "d"),
        ConsoleMessage("info", "i"),
        ConsoleMessage("warning", "w"),
        ConsoleMessage("error", "e é"),
    ]
    assert page.uncaught_errors == []


def test_console_messages_and_errors_keep_a_nul_character(page: Page) -> None:
    page.evaluate("console.log('a\\0b')")
    with pytest.raises(ScriptError) as raised:
        page.evaluate("throw 'c\\0d'")

    assert page.console_messages == [ConsoleMessage("log", "a\0b")]
    assert raised.value.message == "Uncaught c\0d"


def test_a_page_keeps_the_last_of_many_console_messages(page: Page) -> None:
    page.evaluate(f"for (let i = 0; i < {KEPT_REPORTS + 500}; i++) console.log(i)")

    # However many the native core dropped while they waited, a Page keeps the last ones that came.
    assert len(page.console_messages) == KEPT_REPORTS


def test_a_page_keeps_the_errors_that_its_scripts_leave_uncaught(page: Page) -> None:
    assert page.evaluate("Promise.reject(new Error('lost')); Promise.reject(1).catch(() => {}); 'done'") == "done"

    assert page.uncaught_errors == ["Uncaught (in promise) Error: lost"]
    assert page.console_messages == []


@pytest.mark.parametrize(
    ("expression", "timeout", "error", "message"),
    [
        pytest.param(b"1", None, TypeError, "expression has to be text", id="bytes"),
        pytest.param("1", "1", TypeError, "timeout has to be a number of seconds or None, not str", id="timeout-text"),
        pytest.param("1", 0, ValueError, "timeout has to be more than 0", id="timeout-zero"),
        pytest.param("'\ud800'", None, ValueError, "the script holds a lone surrogate", id="lone-surrogate"),
        pytest.param(" " * (_MAX_SCRIPT_SIZE + 1), None, ValueError, "a Page takes at most", id="too-long"),
    ],
)
def test_evaluate_refuses_arguments_that_it_cannot_take(
    page: Page, expression: Any, timeout: Any, error: type[Exception], message: str
) -> None:
    with pytest.raises(error, match=message):
        page.evaluate(expression, timeout=timeout)

    assert page.evaluate("1") == 1


@pytest.mark.parametrize(
    ("url", "error"),
    [pytest.param(b"https://example.com/", TypeError, id="bytes"), pytest.param("https://a\0b/", ValueError, id="nul")],
)
def test_open_page_refuses_a_url_that_it_cannot_take(session: mitto.Session, url: Any, error: type[Exception]) -> None:
    with pytest.raises(error, match="url"):
        _open_page(session, url)


_WITHOUT_THE_RUNTIME = textwrap.dedent(
    """
    import asyncio
    import mitto

    for kind in ("sync", "asyncio"):
        try:
            if kind == "sync":
                with mitto.Session() as session:
                    session._open_page("https://example.com/")
            else:
                async def main():
                    async with mitto.AsyncSession() as session:
                        await session._open_page("https://example.com/")

                asyncio.run(main())
        except mitto.NativeLibraryError as error:
            print(kind, error)
    """
)


def test_a_build_without_the_runtime_says_so(native_library: Any, tmp_path: Path) -> None:
    library_path = _native.find_library()
    assert library_path is not None
    alone = Path(shutil.copy2(library_path, tmp_path))

    result = subprocess.run(
        [sys.executable, "-c", _WITHOUT_THE_RUNTIME],
        env={**os.environ, _native.LIBRARY_ENV_VAR: str(alone)},
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    )

    assert result.returncode == 0, result.stderr
    why = f"the Runtime is not in this build of mitto: no {_native.PAGE_PROGRAM_NAME} lies beside the native library"
    assert result.stdout == f"sync {why}\nasyncio {why}\n"


def test_the_errors_of_a_page_are_mitto_errors_sorted_by_what_a_caller_does_next() -> None:
    for error_type in (ScriptError, PageEndedError, PageTimeoutError):
        assert issubclass(error_type, PageError)
        assert issubclass(error_type, mitto.MittoError)
    assert issubclass(PageTimeoutError, TimeoutError)
    for name in ("PageError", "ScriptError", "PageEndedError", "PageTimeoutError", "ConsoleMessage", "Page"):
        assert name not in mitto.__all__
    assert not hasattr(mitto.Session, "open_page")
    assert not hasattr(mitto.AsyncSession, "open_page")


def test_page_errors_survive_a_pickle() -> None:
    import pickle

    for error in (ScriptError("Uncaught 1"), PageEndedError("gone"), PageTimeoutError(1.5)):
        copy = pickle.loads(pickle.dumps(error))
        assert type(copy) is type(error)
        assert str(copy) == str(error)


@pytest.mark.parametrize(
    ("evaluated", "outcome"),
    [
        pytest.param(PageEvaluated(1, PageErrorCode.NONE, '{"a":1}'), {"a": 1}, id="result"),
        pytest.param(PageEvaluated(1, PageErrorCode.NONE, ""), None, id="nothing"),
        pytest.param(PageEvaluated(1, PageErrorCode.NONE, "{not json"), ScriptError, id="not-json"),
        pytest.param(
            PageEvaluated(1, PageErrorCode.NONE, "[" * 1_000_000 + "]" * 1_000_000),
            ScriptError,
            id="deeper-than-python-reads",
        ),
        pytest.param(PageEvaluated(1, PageErrorCode.SCRIPT, "", "Uncaught 1"), ScriptError, id="script"),
        pytest.param(PageEvaluated(1, PageErrorCode.PROCESS_ENDED, "", "gone"), PageEndedError, id="process-ended"),
        pytest.param(PageEvaluated(1, PageErrorCode.REFUSED, "", "refused"), PageEndedError, id="refused"),
        pytest.param(PageEvaluated(1, PageErrorCode.SESSION_DESTROYED, "", "gone"), PageEndedError, id="session"),
    ],
)
def test_outcome_of_sorts_a_result_by_what_a_caller_does_next(evaluated: PageEvaluated, outcome: Any) -> None:
    if isinstance(outcome, type) and issubclass(outcome, Exception):
        with pytest.raises(outcome):
            outcome_of(evaluated)
    else:
        assert outcome_of(evaluated) == outcome


def test_a_page_record_keeps_only_the_results_that_a_call_waits_for() -> None:
    record = PageRecord()
    record.expect(2)

    record.add(PageEvaluated(1, PageErrorCode.NONE, "1"))
    record.add(PageEvaluated(2, PageErrorCode.NONE, "2"))

    assert record.take(1) is None
    assert record.take(2) == PageEvaluated(2, PageErrorCode.NONE, "2")
    record.forget(2)
    assert record.take(2) is None
    record.add(PageEvaluated(2, PageErrorCode.NONE, "late"))
    assert record.take(2) is None


def test_a_page_record_fails_a_call_with_no_result_once_the_page_has_ended() -> None:
    record = PageRecord()
    record.expect(1)
    record.expect(2)
    record.add(PageEvaluated(1, PageErrorCode.NONE, "1"))

    record.add(PageEnded(PageErrorCode.SESSION_DESTROYED, "the Session was destroyed"))

    assert record.take(1) == PageEvaluated(1, PageErrorCode.NONE, "1")
    assert record.take(2) == PageEvaluated(2, PageErrorCode.SESSION_DESTROYED, "", "the Session was destroyed")


def test_a_page_record_keeps_the_last_reports() -> None:
    record = PageRecord()
    for number in range(KEPT_REPORTS + 1):
        record.add(ConsoleMessage("log", str(number)))
        record.add(UncaughtError(str(number)))

    assert len(record.console_messages) == KEPT_REPORTS
    assert next(iter(record.console_messages)) == ConsoleMessage("log", "1")
    assert len(record.uncaught_errors) == KEPT_REPORTS
    assert next(iter(record.uncaught_errors)) == "1"


def test_a_page_record_keeps_the_last_reports_within_a_limit_on_their_text(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(_page, "KEPT_REPORT_TEXT", 10)
    record = PageRecord()

    for text in ("aaaa", "bbbb", "cccc"):
        record.add(ConsoleMessage("log", text))
    assert [message.text for message in record.console_messages] == ["bbbb", "cccc"]

    record.add(ConsoleMessage("log", "a" * 11))
    assert [message.text for message in record.console_messages] == ["a" * 11]


@pytest.mark.parametrize(
    ("timeout", "seconds"),
    [pytest.param(None, None, id="none"), pytest.param(2, 2.0, id="int"), pytest.param(0.5, 0.5)],
)
def test_to_seconds_takes_a_number_or_none(timeout: float | None, seconds: float | None) -> None:
    assert to_seconds(timeout) == seconds


@pytest.mark.parametrize(
    ("timeout", "error"),
    [
        pytest.param(True, TypeError, id="bool"),
        pytest.param(-1, ValueError, id="negative"),
        pytest.param(float("nan"), ValueError, id="nan"),
        pytest.param(2_000_001, ValueError, id="too-long"),
    ],
)
def test_to_seconds_refuses_what_is_no_timeout(timeout: Any, error: type[Exception]) -> None:
    with pytest.raises(error):
        to_seconds(cast(float, timeout))


def test_the_result_is_what_json_reads(page: Page) -> None:
    value: dict[str, object] = {
        "text": "line\nbreak \u2028 \U0001f600",
        "numbers": [0, -1, 1e21, 0.1],
        "nested": {"empty": []},
    }

    assert page.evaluate(f"({json.dumps(value)})") == value


def test_pages_close_from_other_threads(session: mitto.Session) -> None:
    pages = [_open_page(session) for _ in range(3)]
    closers = [threading.Thread(target=page.close) for page in pages]
    for closer in closers:
        closer.start()
    for closer in closers:
        closer.join(timeout=30)

    for page in pages:
        assert process_is_gone(page.process_id)
