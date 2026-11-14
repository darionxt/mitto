"""Run web-platform-tests in Pages: each test in a Page of its own, on a Session of its own, several at once."""

import json
import time
from collections.abc import Callable, Iterable, Iterator
from concurrent.futures import ThreadPoolExecutor, as_completed

from mitto import Machine, MittoError, RequestError, Session
from mitto._errors import PageEndedError, PageTimeoutError, ScriptError
from tools.wpt.results import CRASH, TIMEOUT, ResultsError, RunResult, result_from_record
from tools.wpt.server import WptServer

__all__ = ["MACHINE", "READ_SECONDS", "run_in_page", "run_in_pages"]

_POLL_SECONDS = 0.02

MACHINE = Machine.draw(seed=0)
"""The Machine of every test's Session, drawn from a fixed seed, so that what a test reads of it, such as the window's
height in a message, is the same in every run and on every Host."""

READ_SECONDS = 2.0
"""How long the runner waits at a test's deadline for the Page to give the results that its page could not post."""


def run_in_page(server: WptServer, test: str, seconds: float) -> RunResult:
    """Run one test in a new Page on a new Session, and return its result, as _run_in_page() does, or a crash with what
    went wrong if the Page or its Session failed in another way, such as a process that could not start."""
    try:
        return _run_in_page(server, test, seconds)
    except (MittoError, OSError) as error:
        return RunResult(test, CRASH, f"the runner: {type(error).__name__}: {error}")


def _run_in_page(server: WptServer, test: str, seconds: float) -> RunResult:
    """Run one test in a new Page on a new Session, and return its result.

    The Page loads the test's URL, as a caller's Page loads a page, and the test posts its results to the server once it
    completes. At the deadline, the runner reads them through evaluate() instead, for a page whose Content Security
    Policy refused the post. A test that has given nothing by then ran out of time, as wptrunner has it.

    Args:
        server: The server, which is running.
        test: The test's address from the root of web-platform-tests.
        seconds: How long the test has, from the start of its Page.
    """
    server.forget(test)
    deadline = time.monotonic() + seconds
    with Session(machine=MACHINE, _settings=server.settings) as session:
        try:
            page = session._open_page(server.url_for(test), timeout=seconds)  # pyright: ignore[reportPrivateUsage]
        except PageTimeoutError:
            return RunResult(test, TIMEOUT)
        except PageEndedError as error:
            return RunResult(test, CRASH, str(error))
        except RequestError as error:
            return RunResult(test, "ERROR", f"the navigation failed: {error}")
        with page:
            while (record := server.posted(test)) is None and time.monotonic() < deadline:
                time.sleep(_POLL_SECONDS)
            if record is None:
                try:
                    text = page.evaluate("self.__mittoResults ?? null", timeout=READ_SECONDS)
                except PageEndedError as error:
                    return RunResult(test, CRASH, str(error))
                except (PageTimeoutError, ScriptError):
                    text = None
                if not isinstance(text, str):
                    return RunResult(test, TIMEOUT)
                record = json.loads(text)
    try:
        return result_from_record(test, record)
    except ResultsError as error:
        return RunResult(test, "ERROR", str(error))


def run_in_pages(
    server: WptServer,
    tests: Iterable[tuple[str, float]],
    jobs: int,
    runner: Callable[[WptServer, str, float], RunResult] = run_in_page,
) -> Iterator[RunResult]:
    """Run tests, each with how many seconds it has, `jobs` at once, and yield each result as it comes, in no order."""
    with ThreadPoolExecutor(jobs) as pool:
        futures = [pool.submit(runner, server, test, seconds) for test, seconds in tests]
        for future in as_completed(futures):
            yield future.result()
