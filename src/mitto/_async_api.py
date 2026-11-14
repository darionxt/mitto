"""The asyncio API: a Session driven from an event loop, on the request building and the native stepping that Session has.

One file descriptor per Session tells the loop when any of its requests or Pages has an event waiting, so 1000 requests
need no thread each. Windows' event loops cannot watch one, so there one thread per Session waits for the same news in the
native core and hands it to the loop. Nothing on the loop waits in the native core. What can block goes to a thread of the
loop's: opening and closing a Session, the file of its cookies, a caller's file or iterable, joining a large body, and
closing a Page, which waits for its process to end.
"""

import asyncio
import concurrent.futures
import functools
import os
import sys
import threading
from collections.abc import AsyncGenerator, AsyncIterable, AsyncIterator, Buffer, Callable, Iterable, Iterator, Sequence
from contextlib import asynccontextmanager
from dataclasses import dataclass
from types import TracebackType
from typing import Any, Self, Unpack, cast

from mitto import _native
from mitto._arguments import require_type
from mitto._body import ReadableFile, StreamBody, async_stream_chunks
from mitto._cookies import AsyncCookies
from mitto._errors import PageTimeoutError, request_error
from mitto._machine import Machine
from mitto._native import (
    WRITE_SIZE,
    BodyAvailable,
    BodyWanted,
    ConsoleMessage,
    Finished,
    NativeEvent,
    NativePage,
    NativeRequest,
    PrivateSettings,
    RedirectFollowed,
    ResponseStarted,
    encode_url,
    finished_response,
)
from mitto._page import (
    KEPT_REPORTS,
    PageNavigation,
    PageRecord,
    loaded_url,
    navigation_of,
    outcome_of,
    take_waiting_events,
)
from mitto._persona import ENGINE_RELEASE_PERSONA
from mitto._request import AsyncRequestOptions, BuiltRequest, RequestOptions
from mitto._response import AsyncResponseStream, Response, join_chunks, to_async_response_stream, to_response
from mitto._session_state import ABOUT_BLANK, SessionState
from mitto._timeout import Timeout, to_seconds

__all__ = ["AsyncPage", "AsyncSession"]

# What a request on a closed Session ends with, as in Session.
_ERR_INVALID_ARGUMENT = -4
_ERR_INVALID_ARGUMENT_NAME = "ERR_INVALID_ARGUMENT"

# Whether a thread that waits in the native core tells the loop of a Session's events, rather than a file descriptor that
# the loop watches. On Windows, ProactorEventLoop has no add_reader(), and SelectorEventLoop watches sockets only.
_WAIT_ON_A_THREAD = sys.platform == "win32"


class _WatchedFd:
    """Has the loop call `on_ready` whenever the event fd of a Session is readable."""

    def __init__(self, fd: int, loop: asyncio.AbstractEventLoop, on_ready: Callable[[], None]) -> None:
        self._fd = fd
        self._loop = loop
        loop.add_reader(fd, on_ready)

    def stop(self) -> Callable[[], None]:
        """Stop watching the fd, which has to happen before the native Session closes it, and return what does nothing."""
        self._loop.remove_reader(self._fd)
        return _do_nothing


class _WaitingThread:
    """Has the loop call `on_ready` from a thread that waits in the native core until a Session's requests have events.

    The native core returns from a wait once each time its list of them fills, and waits again until the loop has emptied
    it, so the thread sleeps for as long as the loop is busy. It waits with the GIL released, and never keeps the
    interpreter from exiting.
    """

    def __init__(self, native: _native.Session, loop: asyncio.AbstractEventLoop, on_ready: Callable[[], None]) -> None:
        self._native = native
        self._loop = loop
        self._on_ready = on_ready
        self._thread = threading.Thread(target=self._wait, name="mitto-session-events", daemon=True)
        self._thread.start()

    def stop(self) -> Callable[[], None]:
        """Return what waits for the thread to end, which it does once the native Session has been detached.

        That has to happen before the native Session closes, and it can block for a moment.
        """
        return self._thread.join

    def _wait(self) -> None:
        while self._native.wait_for_event():
            try:
                self._loop.call_soon_threadsafe(self._on_ready)
            except RuntimeError:
                # The loop has closed with this Session open, and nothing is left on it to wake.
                return


def _do_nothing() -> None:
    pass


class _Events:
    """Wakes each request or Page that waits for an event, when its Session names it.

    The loop hears of events from the Session's event fd, or else from a thread that waits for them in the native core.
    """

    def __init__(self, native: _native.Session, fd: int | None, loop: asyncio.AbstractEventLoop) -> None:
        """Start watching for the events of `native`: on `fd`, or on a thread of its own if that is None."""
        self._native = native
        self._loop = loop
        self._waiting: dict[int, list[asyncio.Future[None]]] = {}
        self._watcher = (
            _WatchedFd(fd, loop, self._on_ready) if fd is not None else _WaitingThread(native, loop, self._on_ready)
        )

    def stop_watching(self) -> Callable[[], None]:
        """Stop watching, and return what waits for that to end, which can block for a moment.

        Both have to happen before the native Session closes, and a thread that waits ends once it has been detached.
        """
        return self._watcher.stop()

    def wake_all(self) -> None:
        """Wake every request and Page that waits, to find the last event that closing the native Session has given each."""
        for waiters in self._waiting.values():
            _wake(waiters)
        self._waiting.clear()

    def wake(self, identity: int) -> None:
        """Wake whatever waits for the request or Page of `identity`, as when it closes."""
        _wake(self._waiting.pop(identity, ()))

    async def ready(self, identity: int) -> None:
        """Return once the Session names the request or Page of `identity` as having an event, or something wakes it.

        Its event may be missing then, or taken already.
        """
        waiter = self._loop.create_future()
        # What goes away with a cancelled task leaves here when the end of its request or Page names it.
        self._waiting.setdefault(identity, []).append(waiter)
        await waiter

    async def next_of(self, request: NativeRequest) -> NativeEvent:
        """Return the next event of `request`, once the loop has had a turn.

        So a server that is as fast as this reads cannot hold up everything else on the loop.

        Raises:
            RequestError: If the Finished event has been taken already, or `request` has been closed.
        """
        event = request.next_event(0)
        if event is not None:
            await asyncio.sleep(0)
            return event
        while event is None:
            await self.ready(request.identity)
            event = request.next_event(0)
        return event

    def _on_ready(self) -> None:
        # Until the list is empty, which is what lets a thread that waits return again. An entry may name a request or Page
        # that has gone, or one that has taken its event already, which next_of() and AsyncPage allow for.
        while (identity := self._native.next_ready()) is not None:
            _wake(self._waiting.pop(identity, ()))


def _wake(waiters: Iterable[asyncio.Future[None]]) -> None:
    for waiter in waiters:
        if not waiter.done():
            waiter.set_result(None)


class _Driver:
    """Takes one request through its events from the event loop, as wait_for_response() and iter_body() do from a thread.

    Once the response has started, this is the async iterator of its body. It is no async generator, because a cancel that
    goes through one ends it, and a later read would then look like the end of the body. So a read that is cancelled cancels
    the native request, and a later read raises a RequestError, whether the caller let the cancel go on or held it back.

    Attributes:
        request: The native request, which its caller closes.
    """

    def __init__(self, request: NativeRequest, events: _Events, body: AsyncIterator[memoryview] | None) -> None:
        self.request = request
        self._events = events
        self._body = body
        self._unsent_body = memoryview(b"")
        self._finished = False

    async def wait_for_response(self) -> tuple[ResponseStarted, tuple[RedirectFollowed, ...]]:
        """Return the final response once it has started, and the redirects that led to it, the first one first.

        Raises:
            RequestError: If the request ends without a response.
        """
        history: list[RedirectFollowed] = []
        while not isinstance(event := await self._next_event(), ResponseStarted):
            if isinstance(event, Finished):
                raise self.request.error(event.net_error)
            if isinstance(event, RedirectFollowed):
                history.append(event)
        return event, tuple(history)

    def __aiter__(self) -> Self:
        return self

    async def __anext__(self) -> bytes:
        """Return the next part of the response's body, which is read when asked for.

        Raises:
            RequestError: If the request ends before the end of the body, also by a read that was cancelled.
            StopAsyncIteration: At the end of the body.
        """
        try:
            return await self._next_chunk()
        except asyncio.CancelledError:
            self.request.cancel()
            raise

    async def _next_chunk(self) -> bytes:
        while not self._finished:
            # None here is what asks for more, which leads to the next event.
            if chunk := self.request.read():
                return chunk
            while not isinstance(event := await self._next_event(), BodyAvailable | Finished):
                pass
            if isinstance(event, Finished):
                self._finished = True
                if event.net_error != 0:
                    raise self.request.error(event.net_error)
        raise StopAsyncIteration

    async def _next_event(self) -> NativeEvent:
        """Return the next event but for a BodyWanted, which this answers."""
        while isinstance(event := await self._events.next_of(self.request), BodyWanted):
            await self._send_body()
        return event

    async def _send_body(self) -> None:
        """Answer a BodyWanted event, as NativeRequest.send_body() does.

        Any part too large for the native core waits for the next event. Only this touches the request here, so cancelling
        what waits for the caller's data can close the request at once.
        """
        if self._body is None:
            return
        while not self._unsent_body.nbytes:
            chunk = await anext(self._body, None)
            if chunk is None:
                self._body = None
                self.request.end_body()
                return
            self._unsent_body = chunk
        written = self.request.write_body(self._unsent_body[:WRITE_SIZE])
        if written == 0:
            self._body = None
        elif written is not None:
            self._unsent_body = self._unsent_body[written:]


class AsyncPage:
    """A Page on an AsyncSession: what Page has, with each call awaited. Close it with an async with statement or close().

    It stays on the event loop of its AsyncSession. The Session's event fd, or its waiting thread on Windows, tells it when
    its events arrive, and closing it waits for its process to end on a thread of its own. Private until the Runtime ships.
    """

    def __init__(self, native: NativePage, url: str, events: _Events, loop: asyncio.AbstractEventLoop) -> None:
        self._native = native
        self._url = url
        self._events = events
        self._loop = loop
        self._record = PageRecord()
        self._closing: asyncio.Future[None] | None = None

    async def __aenter__(self) -> Self:
        return self

    async def __aexit__(
        self, exc_type: type[BaseException] | None, exc: BaseException | None, traceback: TracebackType | None
    ) -> None:
        await self.close()

    @property
    def url(self) -> str:
        """The URL of the Page's document, as Page.url says."""
        self._take_waiting_events()
        return self._record.document_url or self._url

    @property
    def process_id(self) -> int:
        """The ID of the Page's process, which stays the same after the process has ended."""
        return self._native.process_id

    @property
    def console_messages(self) -> list[ConsoleMessage]:
        """What the Page's scripts have written to the console, as Page.console_messages says."""
        self._take_waiting_events()
        return list(self._record.console_messages)

    @property
    def uncaught_errors(self) -> list[str]:
        """What a console says of each error that no script caught, as Page.uncaught_errors says."""
        self._take_waiting_events()
        return list(self._record.uncaught_errors)

    @property
    def known_gaps(self) -> list[str]:
        """The Known Gaps that the Page's documents have reached, as Page.known_gaps says."""
        self._take_waiting_events()
        return list(self._record.known_gaps)

    async def evaluate(self, expression: str, *, timeout: float | None = None) -> Any:
        """Run a script in the Page, and return its result once it has arrived, as Page.evaluate() does.

        Cancelling the task stops the wait, and the script goes on, as a timeout leaves it.

        Raises:
            TypeError: As Page.evaluate() does.
            ValueError: As Page.evaluate() does.
            ScriptError: As Page.evaluate() does.
            PageEndedError: As Page.evaluate() does.
            PageTimeoutError: As Page.evaluate() does.
            RuntimeError: If this runs on another event loop than its AsyncSession's.
        """
        require_type("expression", expression, str, "text")
        seconds = to_seconds(timeout)
        self._require_open()
        call = self._native.evaluate(expression)
        self._record.expect(call)
        try:
            async with asyncio.timeout(seconds):
                evaluated = await self._wait_until(lambda: self._record.take(call))
        except TimeoutError:
            assert seconds is not None
            raise PageTimeoutError(seconds) from None
        finally:
            self._record.forget(call)
        return outcome_of(evaluated)

    async def wait_for_load(self, seconds: float | None) -> None:
        """Return once the document that opening the Page navigated to has loaded, as Page.wait_for_load() does.

        Raises:
            ValueError: As Page.wait_for_load() does.
            RequestError: As Page.wait_for_load() does.
            PageEndedError: As Page.wait_for_load() does.
            PageTimeoutError: As Page.wait_for_load() does.
        """
        self._require_open()
        try:
            async with asyncio.timeout(seconds):
                outcome = await self._wait_until(self._record.load_outcome)
        except TimeoutError:
            assert seconds is not None
            raise PageTimeoutError(seconds) from None
        self._url = loaded_url(self._native, self._url, self._record, outcome)

    async def wait_for_navigation(self, *, timeout: float | None = None) -> PageNavigation:
        """Return how the next navigation that the Page's documents started ended, once it has, as
        Page.wait_for_navigation() does.

        Raises:
            TypeError: As Page.wait_for_navigation() does.
            ValueError: As Page.wait_for_navigation() does.
            RequestError: As Page.wait_for_navigation() does.
            PageEndedError: As Page.wait_for_navigation() does.
            PageTimeoutError: As Page.wait_for_navigation() does.
            RuntimeError: If this runs on another event loop than its AsyncSession's.
        """
        seconds = to_seconds(timeout)
        self._require_open()
        try:
            async with asyncio.timeout(seconds):
                outcome = await self._wait_until(self._record.take_navigation)
        except TimeoutError:
            assert seconds is not None
            raise PageTimeoutError(seconds) from None
        return navigation_of(self._native, outcome)

    async def close(self) -> None:
        """Close the Page, as Page.close() does, off the event loop for the wait, which goes on if the caller is cancelled.

        A task that waits for a result raises a ValueError, and so does each later call. A second call waits for the first.

        Raises:
            RuntimeError: If this runs on another event loop than its AsyncSession's.
        """
        self._require_its_loop()
        if self._closing is None:
            self._events.wake(self._native.identity)
            self._closing = _on_a_thread_of_its_own(self._native.close)
        await asyncio.shield(self._closing)

    def _require_its_loop(self) -> None:
        if asyncio.get_running_loop() is not self._loop:
            raise RuntimeError("an AsyncPage stays on the event loop of its AsyncSession")

    def _require_open(self) -> None:
        self._require_its_loop()
        if self._closing is not None:
            raise ValueError("this Page has been closed")

    async def _wait_until[T](self, outcome: Callable[[], T | None]) -> T:
        """Return what `outcome` gives once it gives something.

        Raises:
            ValueError: If this Page has been closed meanwhile.
        """
        while True:
            self._require_open()
            more = take_waiting_events(self._native, self._record)
            if (result := outcome()) is not None:
                return result
            if more:
                # So that a Page that writes without end cannot hold up everything else on the loop.
                await asyncio.sleep(0)
            else:
                await self._events.ready(self._native.identity)

    def _take_waiting_events(self) -> None:
        if self._closing is None:
            take_waiting_events(self._native, self._record, KEPT_REPORTS)


@dataclass(frozen=True, slots=True)
class _Opened:
    state: SessionState
    events: _Events
    cookies: AsyncCookies


def _on_a_thread_of_its_own[T](function: Callable[[], T]) -> asyncio.Future[T]:
    """Return a future of what `function` returns, off the event loop.

    A thread of the loop's default executor could be late or never come, since a caller's file or iterable may hold them
    all, and what they wait for may be the end of this Session.
    """
    executor = concurrent.futures.ThreadPoolExecutor(max_workers=1)
    future = asyncio.get_running_loop().run_in_executor(executor, function)
    executor.shutdown(wait=False)
    return future


def _create_state(arguments: dict[str, Any]) -> tuple[SessionState, int | None]:
    """Open what can block, and return it with its event fd, or None if a thread is to wait for its events.

    Only events that arrive after the first call for the fd, or the first wait for an event, count, so that comes before
    anything can start a request on it.
    """
    state = SessionState(**arguments)
    try:
        if _WAIT_ON_A_THREAD:
            state.native.wait_for_event(0)
            return state, None
        return state, state.native.event_fd()
    except BaseException:
        state.close()
        raise


async def _create_off_the_loop(arguments: dict[str, Any]) -> tuple[SessionState, int | None]:
    """Return what _create_state() returns, from a thread of its own, which closes it if nobody is left to take it.

    A task that waits here can be cancelled, by its caller or by the end of the event loop, and the thread cannot be.
    """
    created: concurrent.futures.Future[tuple[SessionState, int | None]] = concurrent.futures.Future()

    def create() -> None:
        try:
            result = _create_state(arguments)
        except BaseException as error:
            if created.set_running_or_notify_cancel():
                created.set_exception(error)
            return
        if created.set_running_or_notify_cancel():
            created.set_result(result)
        else:
            result[0].close()

    threading.Thread(target=create, name="mitto-session-open").start()
    try:
        return await asyncio.wrap_future(created)
    except asyncio.CancelledError:
        # Too late for the thread to find out, so it is up to this to close what it made.
        if not created.cancel():
            created.add_done_callback(_close_created)
        raise


def _close_created(created: concurrent.futures.Future[tuple[SessionState, int | None]]) -> None:
    if created.exception() is None:
        threading.Thread(target=created.result()[0].close, name="mitto-session-close").start()


async def _off_the_loop(chunks: Iterator[memoryview]) -> AsyncIterator[memoryview]:
    """Yield what a caller's file or iterable gives, each part from a thread, since it may wait for anything."""
    while (chunk := await asyncio.to_thread(next, chunks, None)) is not None:
        yield chunk


def _without_async_data(options: AsyncRequestOptions) -> tuple[RequestOptions, AsyncIterable[Buffer] | None]:
    """Return `options` as building a request takes them, and any async iterable that they hold as `data`.

    An empty iterable stands in for it, since building a request only asks what kind of body this is. _Driver takes it from
    there.
    """
    data = options.get("data")
    if isinstance(data, AsyncIterable):
        return cast(RequestOptions, {**options, "data": iter(())}), data
    return cast(RequestOptions, options), None


def _body_of(built: BuiltRequest, async_data: AsyncIterable[Buffer] | None) -> AsyncIterator[memoryview] | None:
    """Return what gives the stream body of a built request, or None if it has none."""
    body = built["body"]
    if not isinstance(body, StreamBody):
        return None
    return _off_the_loop(body.chunks) if async_data is None else async_stream_chunks(async_data, body.size)


class AsyncSession:
    """A Session for asyncio: the same browsing state, and the same methods and arguments, with each request awaited.

    Open it with an async with statement. Without one, `await session.open()` opens it and `await session.close()` closes
    it, and a first request opens one that nothing has opened yet. It stays on the event loop that opened it, and belongs to
    that loop's thread.

    Cancelling a task cancels the native request it is waiting for or reading. A Session's arguments and a request's errors
    are those of Session, also on the wire: what differs is how a caller waits.

    Attributes:
        cookies: The cookies, which //net sets from responses and sends with requests. An AsyncSession that has not opened
            has none yet, which raises a RuntimeError.
        machine: The Machine it claims. An AsyncSession that has not opened has none yet, which raises a RuntimeError.
    """

    def __init__(
        self,
        *,
        persona: str = ENGINE_RELEASE_PERSONA,
        machine: Machine | None = None,
        accept_language: str | None = None,
        additional_trust_anchors: Sequence[str] = (),
        timeout: float | Timeout | None = None,
        cookie_file: str | os.PathLike[str] | None = None,
        proxy: str | None = None,
        cache: str | os.PathLike[str] | None = None,
        _settings: PrivateSettings | None = None,
    ) -> None:
        """Keep the arguments for when this opens, which is when they are checked. See Session() for what they mean.

        Args:
            persona: As for Session().
            machine: As for Session(). The Machine is drawn, or read from `cookie_file`, when this opens.
            accept_language: As for Session().
            additional_trust_anchors: As for Session().
            timeout: As for Session().
            cookie_file: As for Session(). It loads and saves on a thread of the loop's.
            proxy: As for Session().
            cache: As for Session().
            _settings: Settings that only tests need, which are not part of the public API.
        """
        # A proxy's URL may hold a password, which Python keeps only until the native core has it.
        self._arguments: dict[str, Any] = {
            "persona": persona,
            "machine": machine,
            "accept_language": accept_language,
            "additional_trust_anchors": additional_trust_anchors,
            "timeout": timeout,
            "cookie_file": cookie_file,
            "proxy": proxy,
            "cache": cache,
            "settings": _settings,
        }
        self._loop: asyncio.AbstractEventLoop | None = None
        self._opening: asyncio.Task[_Opened] | None = None
        self._closing: asyncio.Future[None] | None = None

    async def __aenter__(self) -> Self:
        try:
            return await self.open()
        except asyncio.CancelledError:
            # The opening goes on, on its thread, and no __aexit__ will come.
            self._closing = self._closing or self._start_closing()
            raise

    async def __aexit__(
        self, exc_type: type[BaseException] | None, exc: BaseException | None, traceback: TracebackType | None
    ) -> None:
        await self.close()

    @property
    def cookies(self) -> AsyncCookies:
        """The cookies, as the class says."""
        return self._opened("cookies").cookies

    @property
    def machine(self) -> Machine:
        """The Machine this AsyncSession claims, as Session.machine says. One that has not opened has none yet."""
        return self._opened("Machine").state.machine

    async def open(self) -> Self:
        """Open this AsyncSession on the running event loop, off it for what can block, unless it has opened already.

        It goes on opening if the caller is cancelled, until close() closes it.

        Raises:
            TypeError: As Session() does.
            ValueError: If it has been closed, or else as Session() does.
            OSError: As Session() does.
            NativeLibraryError: If the native library cannot be used.
            RuntimeError: If it opened on another event loop.
        """
        if self._closing is not None:
            raise ValueError("this AsyncSession has been closed")
        self._require_its_loop()
        if self._opening is None:
            self._loop = asyncio.get_running_loop()
            self._opening = asyncio.ensure_future(self._open(self._loop))
        await asyncio.shield(self._opening)
        return self

    async def close(self) -> None:
        """Save the cookies to `cookie_file`, if it has one, and close the Session's connections and any cache on disk.

        This does as Session.close() does, off the event loop for what can block, and goes on if the caller is cancelled.
        Requests still running end with a RequestError, and so do later ones. A second call waits for the first and does
        nothing else.

        Raises:
            OSError: If `cookie_file` cannot be written.
            RuntimeError: If it opened on another event loop.
        """
        self._require_its_loop()
        if self._closing is not None:
            await asyncio.wait({self._closing})
            return
        self._closing = self._start_closing()
        await asyncio.shield(self._closing)

    async def request(self, method: str, url: str, **options: Unpack[AsyncRequestOptions]) -> Response:
        """Fetch `url`, and return once the whole response has arrived.

        Args:
            method: The HTTP method, as for Session.request().
            url: The URL to fetch.
            **options: See AsyncRequestOptions.

        Raises:
            TypeError: As Session.request() does.
            ValueError: As Session.request() does.
            RequestError: As Session.request() does.
            RuntimeError: If this AsyncSession opened on another event loop.
            Exception: Whatever a stream's file or iterable raises, which ends the request.
        """
        driver = await self._start(method, url, options)
        with driver.request:
            started, history = await driver.wait_for_response()
            body = await join_chunks(driver)
        return to_response(finished_response(started, history, body))

    @asynccontextmanager
    async def stream(
        self, method: str, url: str, **options: Unpack[AsyncRequestOptions]
    ) -> AsyncGenerator[AsyncResponseStream]:
        """Fetch `url` in an async with statement, and read the response's body as it arrives.

        Entering the statement waits until the response's headers have arrived, with any stream body gone by then. Leaving
        it ends the request if that is still running.

        Args:
            method: The HTTP method, as for Session.request().
            url: The URL to fetch.
            **options: See AsyncRequestOptions.

        Raises:
            TypeError: As Session.request() does.
            ValueError: As Session.request() does.
            RequestError: As Session.request() does, on entering the statement. An end before the end of the body is raised
                by what reads it.
            RuntimeError: If this AsyncSession opened on another event loop.
            Exception: Whatever a stream's file or iterable raises, which ends the request.
        """
        driver = await self._start(method, url, options)
        with driver.request:
            started, history = await driver.wait_for_response()
            yield to_async_response_stream(started, history, driver)

    async def get(self, url: str, **options: Unpack[AsyncRequestOptions]) -> Response:
        """Send a GET request, as request() does."""
        return await self.request("GET", url, **options)

    async def post(self, url: str, **options: Unpack[AsyncRequestOptions]) -> Response:
        """Send a POST request, as request() does."""
        return await self.request("POST", url, **options)

    async def put(self, url: str, **options: Unpack[AsyncRequestOptions]) -> Response:
        """Send a PUT request, as request() does."""
        return await self.request("PUT", url, **options)

    async def patch(self, url: str, **options: Unpack[AsyncRequestOptions]) -> Response:
        """Send a PATCH request, as request() does."""
        return await self.request("PATCH", url, **options)

    async def delete(self, url: str, **options: Unpack[AsyncRequestOptions]) -> Response:
        """Send a DELETE request, as request() does."""
        return await self.request("DELETE", url, **options)

    async def head(self, url: str, **options: Unpack[AsyncRequestOptions]) -> Response:
        """Send a HEAD request, as request() does."""
        return await self.request("HEAD", url, **options)

    async def options(self, url: str, **options: Unpack[AsyncRequestOptions]) -> Response:
        """Send an OPTIONS request, as request() does."""
        return await self.request("OPTIONS", url, **options)

    async def _open_page(self, url: str, *, timeout: float | None = None) -> AsyncPage:
        """Open a Page for `url`, as Session._open_page() does, on the event loop of this AsyncSession.

        It opens this AsyncSession, if nothing has yet. Starting the Page's process blocks the loop for the few
        milliseconds that it takes. Private until the Runtime ships.

        Raises:
            TypeError: As Session._open_page() does.
            ValueError: As Session._open_page() does, also if this AsyncSession has been closed.
            NativeLibraryError: As Session._open_page() does.
            OSError: As Session._open_page() does.
            RequestError: As Session._open_page() does.
            PageEndedError: As Session._open_page() does.
            PageTimeoutError: As Session._open_page() does.
            RuntimeError: If this AsyncSession opened on another event loop.
        """
        require_type("url", url, str, "text")
        encode_url(url)
        seconds = to_seconds(timeout)
        if self._closing is None:
            await self.open()
        if self._closing is not None or self._opening is None:
            raise ValueError("this AsyncSession has been closed")
        opened = self._opening.result()
        page = AsyncPage(opened.state.open_page(url), url, opened.events, asyncio.get_running_loop())
        if url == ABOUT_BLANK:
            return page
        try:
            await page.wait_for_load(seconds)
        except BaseException:
            await page.close()
            raise
        return page

    def _opened(self, what: str) -> _Opened:
        """Return what opening made.

        Raises:
            RuntimeError: If this has not opened, which names `what` it has none of.
        """
        opening = self._opening
        if opening is None or not opening.done() or opening.cancelled() or opening.exception() is not None:
            raise RuntimeError(f"this AsyncSession has no {what} until it has opened")
        return opening.result()

    def _require_its_loop(self) -> None:
        if self._loop is not None and self._loop is not asyncio.get_running_loop():
            raise RuntimeError("an AsyncSession stays on the event loop that opened it")

    async def _open(self, loop: asyncio.AbstractEventLoop) -> _Opened:
        arguments, self._arguments = self._arguments, {}
        state, fd = await _create_off_the_loop(arguments)
        return _Opened(state, _Events(state.native, fd, loop), AsyncCookies(state.native, state.machine))

    def _start_closing(self) -> asyncio.Future[None]:
        """Return a future of the end of the close.

        This is callbacks and a thread and no task, since the end of the event loop cancels every task, whether or not it
        is shielded, and a close has to get as far as closing the native Session.
        """
        closing = asyncio.get_running_loop().create_future()
        if self._opening is None:
            closing.set_result(None)
        else:
            self._opening.add_done_callback(functools.partial(self._close_opened, closing))
        return closing

    def _close_opened(self, closing: asyncio.Future[None], opening: asyncio.Task[_Opened]) -> None:
        if opening.cancelled() or opening.exception() is not None:
            # It never opened, and what it raised went to whoever asked for it.
            closing.set_result(None)
            return
        opened = opening.result()
        # This all happens in one go on the loop, so nothing there can use a native Session that is closing.
        finish = opened.state.begin_close()
        end_watching = opened.events.stop_watching()

        def close_native() -> None:
            end_watching()
            finish()

        def finished(thread: asyncio.Future[None]) -> None:
            opened.events.wake_all()
            error = None if thread.cancelled() else thread.exception()
            if error is None:
                closing.set_result(None)
            else:
                closing.set_exception(error)

        _on_a_thread_of_its_own(close_native).add_done_callback(finished)

    async def _start(self, method: str, url: str, options: AsyncRequestOptions) -> _Driver:
        """Start a request and return what drives it, which the caller closes through its `request`."""
        if self._closing is None:
            await self.open()
        if self._closing is not None or self._opening is None:
            raise request_error(url, _ERR_INVALID_ARGUMENT, method, _ERR_INVALID_ARGUMENT_NAME)
        opened = self._opening.result()
        request_options, async_data = _without_async_data(options)
        # A file says what size it is through seek(), which may flush it or wait for a disk.
        if isinstance(request_options.get("data"), ReadableFile):
            built = await asyncio.to_thread(opened.state.build, method, url, request_options)
        else:
            built = opened.state.build(method, url, request_options)
        # A native Session that has begun to close by now refuses this with the same error as above.
        return _Driver(opened.state.native.start(**built), opened.events, _body_of(built, async_data))
