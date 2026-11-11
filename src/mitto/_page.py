"""A Page, whose scripts run in a process of their own on the Engine Release's V8, on the Session that opened it.

Private until the Runtime ships: Session._open_page() opens one, and AsyncSession._open_page() an AsyncPage. Until the
Runtime loads documents, a Page holds none: it keeps the URL that it was opened for, fetches nothing, and runs scripts in a
context that has V8's built-ins and console and nothing of a browser yet. docs/runtime.md says more.

What a Page's process sends waits in the native core, as a request's events do. Page takes it with the GIL released, as
the requests of Session do, and AsyncPage when the Session's event fd, or its waiting thread on Windows, names the Page.
"""

import json
import threading
import time
from collections import deque
from collections.abc import Callable, Iterator
from types import TracebackType
from typing import Any, Self

from mitto._arguments import require_type
from mitto._errors import PageEndedError, PageTimeoutError, ScriptError
from mitto._native import (
    ConsoleMessage,
    NativePage,
    NativePageEvent,
    PageEnded,
    PageErrorCode,
    PageEvaluated,
    UncaughtError,
)
from mitto._timeout import to_seconds

__all__ = [
    "EVENTS_PER_TURN",
    "KEPT_REPORTS",
    "KEPT_REPORT_TEXT",
    "ConsoleMessage",
    "Page",
    "PageRecord",
    "outcome_of",
    "take_waiting_events",
]

# How many console messages, and how many uncaught errors, a Page keeps: the last ones, and at most this many characters of
# their text, so that a Page that writes long messages cannot fill the caller's memory. The native core lets as many wait.
KEPT_REPORTS = 1000
KEPT_REPORT_TEXT = 32 * 1024 * 1024

# How many events a call takes at most before it looks for its result again, so that a Page that writes to the console
# without end cannot keep a call from its result or its timeout.
EVENTS_PER_TURN = 100


class LastReports[T]:
    """The last reports of one kind that a Page sent: at most KEPT_REPORTS of them, and KEPT_REPORT_TEXT characters of text.

    The oldest go first, and the newest stays, however long it is.
    """

    def __init__(self, text_of: Callable[[T], str]) -> None:
        self._text_of = text_of
        self._reports: deque[T] = deque()
        self._text = 0

    def __iter__(self) -> Iterator[T]:
        return iter(self._reports)

    def __len__(self) -> int:
        return len(self._reports)

    def append(self, report: T) -> None:
        self._reports.append(report)
        self._text += len(self._text_of(report))
        while len(self._reports) > 1 and (len(self._reports) > KEPT_REPORTS or self._text > KEPT_REPORT_TEXT):
            self._text -= len(self._text_of(self._reports.popleft()))


class PageRecord:
    """What a Page's events have said, sorted for the calls that wait for their results. It neither waits nor locks.

    Attributes:
        console_messages: What the Page's scripts wrote to the console, the oldest first, as LastReports keeps them.
        uncaught_errors: What a console says of each error that no script caught, likewise.
        ended: How the Page ended, once its event has come.
    """

    def __init__(self) -> None:
        # The calls that a caller waits for, with their results once they have arrived.
        self._results: dict[int, PageEvaluated | None] = {}
        self.console_messages: LastReports[ConsoleMessage] = LastReports(lambda message: message.text)
        self.uncaught_errors: LastReports[str] = LastReports(str)
        self.ended: PageEnded | None = None

    def expect(self, call: int) -> None:
        """Keep the result of `call` when it arrives, until take() or forget() asks for it."""
        self._results[call] = None

    def forget(self, call: int) -> None:
        """Stop keeping the result of `call`, which nobody waits for any longer, and drop it if it has arrived."""
        self._results.pop(call, None)

    def take(self, call: int) -> PageEvaluated | None:
        """Return the result of `call` if it has arrived, which forget() then drops, or its failure once the Page has ended.

        Each call that has no result when the Page ends fails as the Page ended, and its own event may come later, or with
        nothing to say that it has, as when the Session has closed.
        """
        if (evaluated := self._results.get(call)) is not None:
            return evaluated
        if self.ended is not None:
            return PageEvaluated(call=call, error=self.ended.error, message=self.ended.message)
        return None

    def add(self, event: NativePageEvent) -> None:
        """Sort an event of the Page. A result that nobody waits for any longer goes."""
        match event:
            case PageEvaluated(call=call) if call in self._results:
                self._results[call] = event
            case ConsoleMessage():
                self.console_messages.append(event)
            case UncaughtError(text=text):
                self.uncaught_errors.append(text)
            case PageEnded():
                self.ended = event
            case PageEvaluated():
                pass


def take_waiting_events(native: NativePage, record: PageRecord, limit: int = EVENTS_PER_TURN) -> bool:
    """Sort the events that wait in the native core, without waiting, up to `limit` of them.

    Returns:
        Whether it stopped at `limit`, with more events perhaps waiting.

    Raises:
        ValueError: If the Page has been closed.
    """
    for _ in range(limit):
        event = native.next_event(0)
        if event is None:
            return False
        record.add(event)
    return True


def outcome_of(evaluated: PageEvaluated) -> Any:
    """Return the result of a call, as Python's json reads it, with None for what JSON.stringify() writes nothing for.

    Raises:
        ScriptError: If the script threw, the promise that it gave was rejected, or its result cannot be read as JSON,
            such as an array nested deeper than Python's json reads.
        PageEndedError: If the Page has ended.
    """
    match evaluated.error:
        case PageErrorCode.NONE:
            if not evaluated.result:
                return None
            try:
                return json.loads(evaluated.result)
            except (ValueError, RecursionError) as error:
                # The Page process writes it, so it may be anything.
                raise ScriptError(f"the result of the script cannot be read as JSON: {error}") from None
        case PageErrorCode.SCRIPT:
            raise ScriptError(evaluated.message)
        case _:
            raise PageEndedError(evaluated.message)


class Page:
    """A Page on a Session, whose scripts run in a process of its own. Close it with close() or a with statement.

    Several threads may use one Page at once: one of them at a time waits in the native core for the Page's events, and
    hands each the result it waits for.
    """

    def __init__(self, native: NativePage, url: str) -> None:
        self._native = native
        self._url = url
        self._record = PageRecord()
        self._lock = threading.Lock()
        self._event_taken = threading.Condition(self._lock)
        # Whether a thread waits in the native core, which close() waits for.
        self._taking = False
        self._closed = False

    def __enter__(self) -> Self:
        return self

    def __exit__(
        self, exc_type: type[BaseException] | None, exc: BaseException | None, traceback: TracebackType | None
    ) -> None:
        self.close()

    @property
    def url(self) -> str:
        """The URL that the Page was opened for, which it fetches nothing from until the Runtime loads documents."""
        return self._url

    @property
    def process_id(self) -> int:
        """The ID of the Page's process, which stays the same after the process has ended."""
        return self._native.process_id

    @property
    def console_messages(self) -> list[ConsoleMessage]:
        """What the Page's scripts have written to the console, the oldest first, for debugging.

        The last 1000 at most, and fewer if their text is longer than 32 Mi characters.
        """
        with self._lock:
            self._take_waiting_events()
            return list(self._record.console_messages)

    @property
    def uncaught_errors(self) -> list[str]:
        """What a console says of each error that no script caught, the oldest first, as console_messages keeps them.

        Such as "Uncaught (in promise) Error: no", for a promise rejected with no handler once a script and the
        microtasks that it queued have run.
        """
        with self._lock:
            self._take_waiting_events()
            return list(self._record.uncaught_errors)

    def evaluate(self, expression: str, *, timeout: float | None = None) -> Any:
        """Run a script in the Page, and block until its result has arrived, with the GIL released while it waits.

        Each script runs as a classic script in one context, after the ones before it, and sees what they left there.

        Args:
            expression: The script.
            timeout: How many seconds to wait for the result, or None to wait for as long as it takes.

        Returns:
            The script's result, as JSON.stringify() writes it and Python's json reads it: None for null and for what
            JSON.stringify() writes nothing for, such as undefined. When it is a promise, this waits until the promise
            settles, and returns what it was fulfilled with. Nothing in a Page can run a timer yet, so a promise that only
            a timer would settle keeps this waiting.

        Raises:
            TypeError: If `expression` is not text, or `timeout` is neither a number nor None.
            ValueError: If `expression` holds a lone surrogate or is longer than a Page takes, which is 16 MiB less 8
                bytes of UTF-8, `timeout` is not a positive number of at most 2,000,000 seconds, or this Page has been
                closed, also by another thread while this waited.
            ScriptError: If the script threw, the promise that it gave was rejected, or its result cannot be read as JSON,
                such as a BigInt.
            PageEndedError: If the Page has ended.
            PageTimeoutError: If `timeout` passed first.
        """
        require_type("expression", expression, str, "text")
        seconds = to_seconds(timeout)
        deadline = None if seconds is None else time.monotonic() + seconds
        with self._lock:
            self._require_open()
            # Under the lock, so that no thread that takes events can take this result before it is expected.
            call = self._native.evaluate(expression)
            self._record.expect(call)
            try:
                evaluated = self._wait_for(call, deadline)
            finally:
                self._record.forget(call)
        if evaluated is None:
            assert seconds is not None
            raise PageTimeoutError(seconds)
        return outcome_of(evaluated)

    def close(self) -> None:
        """Close the Page: end its process, and wait for it to go, which takes up to 2 seconds before a kill.

        A thread that waits for a result raises a ValueError, and so does each later call. A second call does nothing.
        """
        with self._lock:
            if self._closed:
                return
            self._closed = True
            self._native.end_waits()
            self._event_taken.notify_all()
            while self._taking:
                self._event_taken.wait()
            self._native.close()

    def _require_open(self) -> None:
        if self._closed:
            raise ValueError("this Page has been closed")

    def _wait_for(self, call: int, deadline: float | None) -> PageEvaluated | None:
        """Return the result of `call` once it has arrived, or None at `deadline`. Holds the lock, but while it waits.

        Raises:
            ValueError: If this Page has been closed meanwhile.
        """
        while (evaluated := self._record.take(call)) is None:
            self._require_open()
            remaining = None if deadline is None else deadline - time.monotonic()
            if remaining is not None and remaining <= 0:
                return None
            if self._taking:
                # The thread that takes events hands this one its result.
                self._event_taken.wait(remaining)
            else:
                self._take_events(remaining)
        return evaluated

    def _take_events(self, timeout: float | None) -> None:
        """Wait in the native core for the Page's next event, without the lock, and sort what has arrived."""
        self._taking = True
        events: list[NativePageEvent] = []
        self._lock.release()
        try:
            event = self._native.next_event(timeout)
            while event is not None:
                events.append(event)
                event = self._native.next_event(0) if len(events) < EVENTS_PER_TURN else None
        finally:
            self._lock.acquire()
            self._taking = False
            for event in events:
                self._record.add(event)
            self._event_taken.notify_all()

    def _take_waiting_events(self) -> None:
        """Sort what has arrived, without waiting, unless another thread takes the events or the Page has closed."""
        if not self._closed and not self._taking:
            take_waiting_events(self._native, self._record, KEPT_REPORTS)
