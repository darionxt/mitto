"""The asyncio API: a Session driven from an event loop, on the request building and the native stepping that Session has.

One file descriptor per Session tells the loop when any of its requests has an event waiting, so 1000 requests need no
thread each. Nothing on the loop waits in the native core. What can block goes to a thread of the loop's: opening and
closing a Session, the file of its cookies, a caller's file or iterable, and joining a large body.
"""

import asyncio
import concurrent.futures
import functools
import os
import threading
from collections.abc import AsyncGenerator, AsyncIterable, AsyncIterator, Buffer, Callable, Iterable, Iterator, Sequence
from contextlib import asynccontextmanager
from dataclasses import dataclass
from types import TracebackType
from typing import Any, Self, Unpack, cast

from mitto import _native
from mitto._body import ReadableFile, StreamBody, async_stream_chunks
from mitto._cookies import AsyncCookies
from mitto._errors import request_error
from mitto._machine import Machine
from mitto._native import (
    WRITE_SIZE,
    BodyAvailable,
    BodyWanted,
    Finished,
    NativeEvent,
    NativeRequest,
    PrivateSettings,
    RedirectFollowed,
    ResponseStarted,
    finished_response,
)
from mitto._persona import ENGINE_RELEASE_PERSONA
from mitto._request import AsyncRequestOptions, BuiltRequest, RequestOptions
from mitto._response import AsyncResponseStream, Response, join_chunks, to_async_response_stream, to_response
from mitto._session_state import SessionState
from mitto._timeout import Timeout

__all__ = ["AsyncSession"]

# What a request on a closed Session ends with, as in Session.
_ERR_INVALID_ARGUMENT = -4
_ERR_INVALID_ARGUMENT_NAME = "ERR_INVALID_ARGUMENT"


class _Events:
    """Wakes each request that waits for an event, when the event fd of its Session names it."""

    def __init__(self, native: _native.Session, fd: int, loop: asyncio.AbstractEventLoop) -> None:
        self._native = native
        self._fd = fd
        self._loop = loop
        self._waiting: dict[int, list[asyncio.Future[None]]] = {}
        loop.add_reader(fd, self._on_readable)

    def stop_watching(self) -> None:
        """Stop watching the fd, which has to happen before the native Session closes it."""
        self._loop.remove_reader(self._fd)

    def wake_all(self) -> None:
        """Wake every request that waits, to find the last event that closing the native Session has given each."""
        for waiters in self._waiting.values():
            _wake(waiters)
        self._waiting.clear()

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
            waiter = self._loop.create_future()
            # What goes away with a cancelled task leaves here when the end of its request names it.
            self._waiting.setdefault(request.identity, []).append(waiter)
            await waiter
            event = request.next_event(0)
        return event

    def _on_readable(self) -> None:
        # An entry may name a request that has gone, or one that has taken its event already, which next_of() allows for.
        while (identity := self._native.next_ready_request()) is not None:
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


def _create_state(arguments: dict[str, Any]) -> tuple[SessionState, int]:
    """Open what can block, and return it with its event fd, before anything can start a request on it.

    Only events that arrive after the first call for the fd count.
    """
    state = SessionState(**arguments)
    try:
        return state, state.native.event_fd()
    except BaseException:
        state.close()
        raise


async def _create_off_the_loop(arguments: dict[str, Any]) -> tuple[SessionState, int]:
    """Return what _create_state() returns, from a thread of its own, which closes it if nobody is left to take it.

    A task that waits here can be cancelled, by its caller or by the end of the event loop, and the thread cannot be.
    """
    created: concurrent.futures.Future[tuple[SessionState, int]] = concurrent.futures.Future()

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


def _close_created(created: concurrent.futures.Future[tuple[SessionState, int]]) -> None:
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
        opened.events.stop_watching()

        def finished(thread: asyncio.Future[None]) -> None:
            opened.events.wake_all()
            error = None if thread.cancelled() else thread.exception()
            if error is None:
                closing.set_result(None)
            else:
                closing.set_exception(error)

        _on_a_thread_of_its_own(finish).add_done_callback(finished)

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
