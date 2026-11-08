"""What only an AsyncSession has: an event loop to keep moving, tasks to cancel, and an await where a Session blocks.

The feature tests go through it too, by the both_apis fixture in tests/conftest.py. These go through the real native core to
local servers, so they need no network connection, and are skipped when this machine has no native library. Each goes
through every way that an event loop can hear of a Session's events on this Host, which tests/event_loops.py names.
"""

import asyncio
import http.server
import inspect
import io
import os
import subprocess
import sys
import textwrap
import threading
import time
from collections.abc import AsyncIterator, Callable, Coroutine, Iterator
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path
from typing import Any, cast, get_type_hints

import pytest

import mitto
from mitto import _async_api, _cookie_file, _native
from mitto._request import AsyncRequestOptions, RequestOptions
from mitto._session_state import SessionState
from tests import event_loops
from tests.local_server import QuietServer
from tools.capture import CASES, HTTP2, CaptureServer, normal_form
from tools.parity.replay import run_of, session_settings

pytestmark = pytest.mark.usefixtures("native_library")

ERR_ABORTED = -3
ERR_INVALID_ARGUMENT = -4
ERR_ALPN_NEGOTIATION_FAILED = -122
ERR_UPLOAD_STREAM_REWIND_NOT_SUPPORTED = -25

WAIT_SECONDS = 30.0
BODY = b"the whole body of a response"
PARTS = [b"first ", b"second ", b"third"]
WHOLE = b"".join(PARTS)

# More than a write gives the native core at once.
LARGE_PART = bytes(range(256)) * 1024

# For a test to wait on while the thread of an event loop's default executor is taken.
_OTHER_THREAD = ThreadPoolExecutor(max_workers=1)

# The name of the thread that waits for a Session's events, where one does.
WAITING_THREAD = "mitto-session-events"


@pytest.fixture(autouse=True, params=event_loops.KINDS)
def kind_of_loop(request: pytest.FixtureRequest, monkeypatch: pytest.MonkeyPatch) -> str:
    """Run each test once for each way that an event loop can hear of a Session's events, and return its name."""
    event_loops.use(request.param, monkeypatch)
    return request.param


@pytest.fixture
def waiting_thread_only(kind_of_loop: str) -> None:
    if not event_loops.waits_on_a_thread(kind_of_loop):
        pytest.skip("the loop watches a file descriptor, and no thread waits")


class Server(QuietServer):
    """An HTTP/1.1 server whose paths answer in a way that tests can watch."""

    # Several Sessions open their connections at once, and the default of 5 resets some of them, as it does for a Session.
    request_queue_size = 128

    def __init__(self) -> None:
        super().__init__(("127.0.0.1", 0), Handler)
        self.received: list[tuple[str, str, bytes]] = []
        self.client_arrived = threading.Event()
        self.client_left = threading.Event()
        self.answer = threading.Event()

    def url(self, path: str = "/") -> str:
        return f"http://127.0.0.1:{self.server_port}{path}"


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        server = cast(Server, self.server)
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        server.received.append((self.command, self.path, body))
        if self.path == "/silent":
            server.client_arrived.set()
            # Only a client that goes away ends this: recv() returns nothing then.
            self.connection.recv(1)
            server.client_left.set()
            self.close_connection = True
        elif self.path == "/endless":
            self.send_head(1 << 40)
            try:
                while True:
                    self.wfile.write(bytes(64 * 1024))
            except OSError:
                server.client_left.set()
                self.close_connection = True
        elif self.path == "/held":
            self.send_head(len(BODY))
            self.wfile.flush()
            server.answer.wait(WAIT_SECONDS)
            self.wfile.write(BODY)
        elif self.path == "/slow-body":
            self.send_head(len(BODY))
            self.wfile.flush()
            # Only a client that goes away ends this: recv() returns nothing then.
            self.connection.recv(1)
            server.client_left.set()
            self.close_connection = True
        elif self.path == "/see-other":
            self.send_redirect(303)
        elif self.path == "/temporary":
            self.send_redirect(307)
        elif self.path.startswith("/echo/"):
            self.send_body(self.path.encode())
        else:
            self.send_body(BODY)

    do_POST = do_GET  # noqa: N815

    def send_head(self, size: int) -> None:
        self.send_response(200)
        self.send_header("Content-Length", str(size))
        self.send_header("Content-Type", "text/plain")
        self.end_headers()

    def send_body(self, body: bytes) -> None:
        self.send_head(len(body))
        self.wfile.write(body)

    def send_redirect(self, status: int) -> None:
        self.send_response(status)
        self.send_header("Location", "/body")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, format: str, *args: Any) -> None:
        pass


@pytest.fixture
def server() -> Iterator[Server]:
    with Server() as running:
        threading.Thread(target=running.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True).start()
        yield running
        running.answer.set()
        running.shutdown()


def run[T](coroutine: Coroutine[Any, Any, T]) -> T:
    """Run a test's coroutine on an event loop of its own, which a hang or an error in a callback fails rather than stops."""
    errors: list[dict[str, Any]] = []

    async def limited() -> T:
        asyncio.get_running_loop().set_exception_handler(lambda _, context: errors.append(context))
        async with asyncio.timeout(WAIT_SECONDS):
            return await coroutine

    result = asyncio.run(limited(), loop_factory=event_loops.new_event_loop)
    assert not errors
    return result


class HeldOpening:
    """Stands in for what opens a Session, and holds it up until a test says so.

    Attributes:
        started: Set once a thread is opening a Session.
        go_on: What a test sets to let it go on.
        made: Set once the Session exists.
    """

    def __init__(self) -> None:
        self.started = threading.Event()
        self.go_on = threading.Event()
        self.made = threading.Event()
        self._create = _async_api.SessionState

    def __call__(self, **arguments: Any) -> SessionState:
        self.started.set()
        self.go_on.wait(WAIT_SECONDS)
        state = self._create(**arguments)
        self.made.set()
        return state

    def let_go_and_wait(self) -> None:
        """Let the Session open. Another may not try for its cache before this one has it, or it would find the cache free."""
        self.go_on.set()
        assert self.made.wait(WAIT_SECONDS)


@pytest.fixture
def held_opening(monkeypatch: pytest.MonkeyPatch) -> HeldOpening:
    held = HeldOpening()
    monkeypatch.setattr(_async_api, "SessionState", held)
    return held


def opened_state(session: mitto.AsyncSession) -> SessionState:
    opening = session._opening  # pyright: ignore[reportPrivateUsage]
    assert opening is not None
    return opening.result().state


async def wait_for(event: threading.Event) -> bool:
    return await asyncio.to_thread(event.wait, WAIT_SECONDS)


async def parts_of(*parts: bytes) -> AsyncIterator[bytes]:
    for part in parts:
        await asyncio.sleep(0)
        yield part


# The methods and arguments.


def _public_names(cls: type) -> set[str]:
    return {name for name in vars(cls) if not name.startswith("_")}


def test_it_has_each_method_of_a_session() -> None:
    assert _public_names(mitto.Session) <= _public_names(mitto.AsyncSession)


@pytest.mark.parametrize("name", [*sorted(_public_names(mitto.Session)), "__init__"])
def test_each_method_takes_the_arguments_of_a_session(name: str) -> None:
    def shape(cls: type) -> list[tuple[str, Any, Any]]:
        member: Any = getattr(cls, name)
        if isinstance(member, property):
            member = member.fget
        parameters = inspect.signature(member).parameters.values()
        return [(parameter.name, parameter.kind, parameter.default) for parameter in parameters]

    assert shape(mitto.AsyncSession) == shape(mitto.Session)


def test_a_request_takes_the_options_of_a_session() -> None:
    assert set(get_type_hints(AsyncRequestOptions)) == set(get_type_hints(RequestOptions))


def test_a_session_refuses_an_async_iterable(server: Server) -> None:
    with mitto.Session() as session, pytest.raises(TypeError, match="data has to be"):
        session.post(server.url(), data=parts_of(b"part"))  # pyright: ignore[reportCallIssue, reportArgumentType]


# A whole response, and one as it arrives.


def test_a_request_is_awaited(server: Server) -> None:
    async def main() -> mitto.Response:
        async with mitto.AsyncSession() as session:
            return await session.get(server.url("/body"))

    response = run(main())

    assert (response.status, response.content, response.protocol) == (200, BODY, "http/1.1")
    assert response.text == BODY.decode()
    assert response.headers["Content-Type"] == "text/plain"


def test_a_stream_is_read_as_it_arrives(server: Server) -> None:
    async def main() -> tuple[int, bytes, bytes]:
        async with mitto.AsyncSession() as session:
            async with session.stream("GET", server.url("/body")) as response:
                chunks = [chunk async for chunk in response.iter_bytes()]
            async with session.stream("GET", server.url("/body")) as response:
                return response.status, b"".join(chunks), await response.read()

    assert run(main()) == (200, BODY, BODY)


def test_reading_after_the_statement_raises(server: Server) -> None:
    async def main() -> None:
        async with mitto.AsyncSession() as session:
            async with session.stream("GET", server.url("/endless")) as response:
                pass
            with pytest.raises(mitto.RequestError):
                await response.read()

    run(main())
    assert server.client_left.wait(WAIT_SECONDS)


def test_leaving_the_statement_ends_the_request(server: Server) -> None:
    async def main() -> None:
        async with mitto.AsyncSession() as session, session.stream("GET", server.url("/endless")) as response:
            assert await anext(response.iter_bytes())

    run(main())
    assert server.client_left.wait(WAIT_SECONDS)


def test_a_body_above_what_a_loop_joins_itself_is_joined_on_a_thread(
    server: Server, monkeypatch: pytest.MonkeyPatch
) -> None:
    called: list[Any] = []
    to_thread = asyncio.to_thread

    async def recorded(function: Any, /, *args: Any, **kwargs: Any) -> Any:
        called.append(function)
        return await to_thread(function, *args, **kwargs)

    async def main() -> bytes:
        async with mitto.AsyncSession() as session:
            monkeypatch.setattr(asyncio, "to_thread", recorded)
            content = (await session.get(server.url("/body"))).content
            monkeypatch.undo()
            return content

    monkeypatch.setattr("mitto._response._MOST_BYTES_TO_JOIN_ON_A_LOOP", 1)
    assert run(main()) == BODY
    assert called == [b"".join]


def test_a_small_body_is_joined_on_the_loop(server: Server, monkeypatch: pytest.MonkeyPatch) -> None:
    async def main() -> bytes:
        async with mitto.AsyncSession() as session:

            def fail(*args: Any) -> None:
                pytest.fail("a thread is not worth it for this little")

            monkeypatch.setattr(asyncio, "to_thread", fail)
            content = (await session.get(server.url("/body"))).content
            monkeypatch.undo()
            return content

    assert run(main()) == BODY


def test_concurrent_requests_each_get_their_own_response(server: Server) -> None:
    count = 200

    async def main() -> list[bytes]:
        async with mitto.AsyncSession() as session:
            responses = await asyncio.gather(*(session.get(server.url(f"/echo/{i}")) for i in range(count)))
            return [response.content for response in responses]

    assert run(main()) == [f"/echo/{i}".encode() for i in range(count)]


def test_two_sessions_on_one_loop_keep_their_events_apart(server: Server) -> None:
    async def main() -> list[bytes]:
        async with mitto.AsyncSession() as first, mitto.AsyncSession() as second:
            responses = await asyncio.gather(
                *((first if i % 2 else second).get(server.url(f"/echo/{i}")) for i in range(40))
            )
            return [response.content for response in responses]

    assert run(main()) == [f"/echo/{i}".encode() for i in range(40)]


# The loop gets its turn.


def test_a_request_that_always_has_an_event_waiting_still_gives_the_loop_a_turn(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """A server as fast as this reads could otherwise keep a request from ever waiting, and so the loop from running."""

    class AlwaysAnEvent:
        identity = 1

        def next_event(self, timeout: float) -> _native.NativeEvent:
            return _native.BodyAvailable()

    async def main() -> int:
        turns = 0

        async def count_turns() -> None:
            nonlocal turns
            while True:
                turns += 1
                await asyncio.sleep(0)

        counting = asyncio.create_task(count_turns())
        async with mitto.AsyncSession() as session:
            events = session._opening.result().events  # pyright: ignore[reportPrivateUsage, reportOptionalMemberAccess]
            request = cast(_native.NativeRequest, AlwaysAnEvent())
            turns = 0
            for _ in range(100):
                await events.next_of(request)
            seen = turns
        counting.cancel()
        return seen

    assert run(main()) >= 100


# A thread that waits for the events.

# How long a test keeps its loop busy while events arrive.
BUSY_SECONDS = 1.0


def waiting_thread(session: mitto.AsyncSession) -> threading.Thread:
    opening = session._opening  # pyright: ignore[reportPrivateUsage]
    assert opening is not None
    watcher = opening.result().events._watcher  # pyright: ignore[reportPrivateUsage]
    assert isinstance(watcher, _async_api._WaitingThread)  # pyright: ignore[reportPrivateUsage]
    return watcher._thread  # pyright: ignore[reportPrivateUsage]


@pytest.mark.usefixtures("waiting_thread_only")
def test_each_session_has_a_thread_of_its_own_that_ends_with_it(server: Server) -> None:
    async def main() -> None:
        async with mitto.AsyncSession() as first, mitto.AsyncSession() as second:
            threads = {waiting_thread(first), waiting_thread(second)}
            assert len(threads) == 2
            for thread in threads:
                assert (thread.name, thread.daemon, thread.is_alive()) == (WAITING_THREAD, True, True)
            assert (await first.get(server.url("/body"))).content == BODY
        assert not any(thread.is_alive() for thread in threads)

    run(main())


@pytest.mark.usefixtures("waiting_thread_only")
def test_the_waiting_thread_ends_before_the_native_session_closes(
    server: Server, monkeypatch: pytest.MonkeyPatch
) -> None:
    threads: list[threading.Thread] = []
    alive_at_close: list[bool] = []
    detach = _native.Session.detach

    def recorded(self: _native.Session) -> Callable[[], None]:
        destroy = detach(self)

        def checked() -> None:
            alive_at_close.extend(thread.is_alive() for thread in threads)
            destroy()

        return checked

    monkeypatch.setattr(_native.Session, "detach", recorded)

    async def main() -> None:
        async with mitto.AsyncSession() as session:
            threads.append(waiting_thread(session))
            assert (await session.get(server.url("/body"))).content == BODY

    run(main())
    assert alive_at_close == [False]


@pytest.mark.usefixtures("waiting_thread_only")
def test_the_waiting_thread_sleeps_while_events_wait_for_a_busy_loop(
    server: Server, monkeypatch: pytest.MonkeyPatch
) -> None:
    """However many events arrive while the loop is busy, the thread wakes the loop once and then sleeps until it drains."""
    # When each wait on the thread returned, by the clock and by the thread's own CPU time. The clock is perf_counter(),
    # since monotonic() on Windows only moves every 15.6 ms.
    returns: list[tuple[float, float]] = []
    taken: list[int] = []
    wait_for_event = _native.Session.wait_for_event
    next_ready_request = _native.Session.next_ready_request

    def timed(self: _native.Session, timeout: float | None = None) -> bool:
        woke = wait_for_event(self, timeout)
        if threading.current_thread().name == WAITING_THREAD:
            returns.append((time.perf_counter(), time.thread_time()))
        return woke

    def counted(self: _native.Session) -> int | None:
        identity = next_ready_request(self)
        if identity is not None:
            taken.append(identity)
        return identity

    monkeypatch.setattr(_native.Session, "wait_for_event", timed)
    monkeypatch.setattr(_native.Session, "next_ready_request", counted)
    count = 20

    async def main() -> tuple[float, float, int]:
        async with mitto.AsyncSession() as session:
            assert (await session.get(server.url("/body"))).content == BODY
            requests = [asyncio.create_task(session.get(server.url(f"/echo/{i}"))) for i in range(count)]
            # Each starts its native request, and waits for its first event.
            await asyncio.sleep(0)
            busy_from = time.perf_counter()
            # On purpose this holds up the loop, while the responses arrive.
            time.sleep(BUSY_SECONDS)
            busy_until = time.perf_counter()
            taken_before = len(taken)
            await asyncio.sleep(0)
            taken_at_once = len(taken) - taken_before
            responses = await asyncio.gather(*requests)
            assert [response.content for response in responses] == [f"/echo/{i}".encode() for i in range(count)]
        return busy_from, busy_until, taken_at_once

    busy_from, busy_until, taken_at_once = run(main())
    returns_while_busy = sum(busy_from <= at <= busy_until for at, _ in returns)
    thread_seconds = returns[-1][1]
    print(
        f"loop busy {busy_until - busy_from:.3f} s; waits that returned then: {returns_while_busy}; "
        f"events the loop took at once after: {taken_at_once}; waits in all: {len(returns)}; "
        f"CPU of the waiting thread in all: {thread_seconds:.4f} s"
    )
    assert returns_while_busy <= 1
    assert taken_at_once > 1
    assert thread_seconds < BUSY_SECONDS / 10


@pytest.mark.usefixtures("waiting_thread_only")
def test_an_event_after_its_loop_has_closed_ends_the_waiting_thread_quietly(
    server: Server, monkeypatch: pytest.MonkeyPatch
) -> None:
    raised: list[threading.ExceptHookArgs] = []
    monkeypatch.setattr(threading, "excepthook", raised.append)

    async def main() -> tuple[SessionState, threading.Thread]:
        session = await mitto.AsyncSession().open()
        return opened_state(session), waiting_thread(session)

    state, thread = run(main())
    # The loop has closed with the Session open, and a request on it now has events that no loop takes.
    with state.native.start(server.url("/body")) as request:
        assert request.next_event(WAIT_SECONDS) is not None
        thread.join(WAIT_SECONDS)
    state.close()

    assert not thread.is_alive()
    assert raised == []


# Opening.


def test_a_first_request_opens_it_once_and_close_closes_it(server: Server, monkeypatch: pytest.MonkeyPatch) -> None:
    created: list[SessionState] = []
    create = _async_api.SessionState

    def counted(**arguments: Any) -> SessionState:
        created.append(create(**arguments))
        return created[-1]

    monkeypatch.setattr(_async_api, "SessionState", counted)

    async def main() -> list[int]:
        session = mitto.AsyncSession()
        responses = await asyncio.gather(*(session.get(server.url("/body")) for _ in range(5)))
        await session.close()
        return [response.status for response in responses]

    assert run(main()) == [200] * 5
    assert len(created) == 1


def test_open_returns_the_session_and_a_second_open_changes_nothing(server: Server) -> None:
    async def main() -> None:
        session = await mitto.AsyncSession().open()
        state = opened_state(session)
        assert await session.open() is session
        assert opened_state(session) is state
        await session.close()

    run(main())


def test_it_can_be_made_before_any_loop_runs(server: Server) -> None:
    session = mitto.AsyncSession()

    async def main() -> bytes:
        async with session:
            return (await session.get(server.url("/body"))).content

    assert run(main()) == BODY


def test_it_has_no_cookies_until_it_has_opened() -> None:
    async def main() -> None:
        session = mitto.AsyncSession()
        with pytest.raises(RuntimeError, match="until it has opened"):
            _ = session.cookies
        async with session:
            assert len(session.cookies) == 0

    run(main())


def test_it_has_no_machine_until_it_has_opened_and_then_the_one_it_was_given() -> None:
    machine = mitto.Machine.draw(seed=9)

    async def main() -> None:
        session = mitto.AsyncSession(machine=machine)
        with pytest.raises(RuntimeError, match="no Machine until it has opened"):
            _ = session.machine
        async with session:
            assert session.machine is machine

    run(main())


@pytest.mark.parametrize(
    ("arguments", "error"),
    [
        ({"persona": "no such persona"}, ValueError),
        ({"timeout": -1}, ValueError),
        ({"proxy": "ftp://proxy"}, ValueError),
        ({"cache": ""}, ValueError),
    ],
    ids=["persona", "timeout", "proxy", "cache"],
)
def test_a_session_that_cannot_open_raises_there_each_time(arguments: dict[str, Any], error: type[Exception]) -> None:
    async def main() -> None:
        session = mitto.AsyncSession(**arguments)
        with pytest.raises(error):
            await session.open()
        with pytest.raises(error):
            await session.get("http://127.0.0.1:1/")
        with pytest.raises(RuntimeError, match="until it has opened"):
            _ = session.cookies
        await session.close()

    run(main())


def test_a_session_that_cannot_open_says_so_in_an_async_with_statement() -> None:
    async def main() -> None:
        with pytest.raises(ValueError, match="persona"):
            async with mitto.AsyncSession(persona="no such persona"):
                pytest.fail("it should not have opened")

    run(main())


def test_it_keeps_no_proxy_url_once_it_has_opened(server: Server) -> None:
    password = "a-secret-password"

    async def main() -> None:
        async with mitto.AsyncSession(proxy=f"http://user:{password}@127.0.0.1:1") as session:
            assert password not in repr(vars(session))

    run(main())


def test_it_stays_on_the_loop_that_opened_it(server: Server) -> None:
    session = mitto.AsyncSession()
    run(session.open())

    async def elsewhere() -> None:
        with pytest.raises(RuntimeError, match="stays on the event loop"):
            await session.get(server.url("/body"))
        with pytest.raises(RuntimeError, match="stays on the event loop"):
            await session.close()

    run(elsewhere())
    opened_state(session).close()


# What can block stays off the loop.


def test_opening_and_closing_block_only_a_thread_of_the_loop(
    server: Server, monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    threads: dict[str, int] = {}
    native_session = _native.Session
    replace_file = _cookie_file._replace_file  # pyright: ignore[reportPrivateUsage]
    detach = _native.Session.detach

    def created(**settings: Any) -> _native.Session:
        threads["create"] = threading.get_ident()
        return native_session(**settings)

    def detached(self: _native.Session) -> Callable[[], None]:
        destroy = detach(self)

        def counted() -> None:
            threads["destroy"] = threading.get_ident()
            destroy()

        return counted

    def replaced(path: Path, contents: bytes) -> None:
        threads["save"] = threading.get_ident()
        replace_file(path, contents)

    monkeypatch.setattr(_native, "Session", created)
    monkeypatch.setattr(native_session, "detach", detached)
    monkeypatch.setattr(_cookie_file, "_replace_file", replaced)

    async def main() -> None:
        async with mitto.AsyncSession(cookie_file=tmp_path / "cookies.json"):
            pass

    run(main())
    loop_thread = threading.get_ident()
    assert set(threads) == {"create", "destroy", "save"}
    assert loop_thread not in threads.values()


def test_a_session_that_takes_long_to_open_and_close_leaves_the_loop_moving(monkeypatch: pytest.MonkeyPatch) -> None:
    pause = 0.2
    native_session = _native.Session
    detach = _native.Session.detach

    def created(**settings: Any) -> _native.Session:
        time.sleep(pause)
        return native_session(**settings)

    def detached(self: _native.Session) -> Callable[[], None]:
        destroy = detach(self)

        def slow() -> None:
            time.sleep(pause)
            destroy()

        return slow

    monkeypatch.setattr(_native, "Session", created)
    monkeypatch.setattr(native_session, "detach", detached)

    async def main() -> int:
        ticks = 0

        async def tick() -> None:
            nonlocal ticks
            while True:
                await asyncio.sleep(0.01)
                ticks += 1

        ticking = asyncio.create_task(tick())
        async with mitto.AsyncSession():
            pass
        ticking.cancel()
        return ticks

    # A loop that the pauses held up would have ticked a few times at most.
    assert run(main()) > 10


def test_cookies_go_through_a_file_off_the_loop(
    server: Server, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    threads: list[int] = []
    replace_file = _cookie_file._replace_file  # pyright: ignore[reportPrivateUsage]
    read_bytes = Path.read_bytes

    def replaced(path: Path, contents: bytes) -> None:
        threads.append(threading.get_ident())
        replace_file(path, contents)

    def read(path: Path) -> bytes:
        threads.append(threading.get_ident())
        return read_bytes(path)

    monkeypatch.setattr(_cookie_file, "_replace_file", replaced)
    monkeypatch.setattr(Path, "read_bytes", read)
    path = tmp_path / "cookies.json"

    async def main() -> list[str]:
        async with mitto.AsyncSession() as first:
            first.cookies.set(mitto.Cookie("a", "1", domain="example.com", secure=True))
            await first.cookies.save(path)
            machine = first.machine
        async with mitto.AsyncSession(machine=machine) as second:
            await second.cookies.load(path)
            return [cookie.name for cookie in second.cookies]

    assert run(main()) == ["a"]
    assert len(threads) == 2
    assert threading.get_ident() not in threads


def test_a_file_that_holds_no_cookies_is_refused_and_named(tmp_path: Path) -> None:
    path = tmp_path / "cookies.json"
    path.write_bytes(b"not cookies")

    async def main() -> None:
        async with mitto.AsyncSession() as session:
            with pytest.raises(ValueError, match="does not hold cookies"):
                await session.cookies.load(path)

    run(main())


def test_a_cookie_file_loads_on_opening_and_saves_on_closing(tmp_path: Path) -> None:
    path = tmp_path / "cookies.json"

    async def main() -> list[str]:
        async with mitto.AsyncSession(cookie_file=path) as first:
            first.cookies.set(mitto.Cookie("a", "1", domain="example.com", secure=True))
        async with mitto.AsyncSession(cookie_file=path) as second:
            return [cookie.name for cookie in second.cookies]

    assert run(main()) == ["a"]


# Closing.


def test_a_second_close_does_nothing(tmp_path: Path) -> None:
    async def main() -> None:
        session = await mitto.AsyncSession(cookie_file=tmp_path / "missing" / "cookies.json").open()
        with pytest.raises(OSError, match="missing"):
            await session.close()
        await session.close()

    run(main())


def test_a_close_with_a_file_it_cannot_write_still_closes_the_connections(tmp_path: Path, server: Server) -> None:
    async def main() -> None:
        session = await mitto.AsyncSession(cookie_file=tmp_path / "missing" / "cookies.json").open()
        running = asyncio.create_task(session.get(server.url("/silent")))
        assert await wait_for(server.client_arrived)
        with pytest.raises(OSError, match="missing"):
            await session.close()
        with pytest.raises(mitto.RequestError) as caught:
            await running
        assert caught.value.net_error == ERR_ABORTED

    run(main())
    assert server.client_left.wait(WAIT_SECONDS)


def test_closing_what_never_opened_opens_nothing(monkeypatch: pytest.MonkeyPatch) -> None:
    def fail(**arguments: Any) -> None:
        pytest.fail("this Session should not have opened")

    monkeypatch.setattr(_async_api, "SessionState", fail)

    async def main() -> None:
        session = mitto.AsyncSession()
        await session.close()
        with pytest.raises(mitto.RequestError) as caught:
            await session.get("http://127.0.0.1:1/")
        assert caught.value.net_error == ERR_INVALID_ARGUMENT
        with pytest.raises(ValueError, match="has been closed"):
            await session.open()

    run(main())


def test_a_later_request_and_cookie_call_are_refused_as_on_a_session(server: Server) -> None:
    async def main() -> None:
        session = await mitto.AsyncSession().open()
        cookies = session.cookies
        await session.close()
        with pytest.raises(mitto.RequestError) as caught:
            await session.get(server.url("/body"))
        assert caught.value.net_error == ERR_INVALID_ARGUMENT
        with pytest.raises(mitto.RequestError):
            async with session.stream("GET", server.url("/body")):
                pytest.fail("it should not have started")
        with pytest.raises(ValueError, match="has been closed"):
            len(cookies)
        with pytest.raises(ValueError, match="has been closed"):
            await cookies.save("unused")

    run(main())
    assert not server.received


def test_a_request_or_cookie_call_during_the_close_is_refused(server: Server, monkeypatch: pytest.MonkeyPatch) -> None:
    """Nothing on the loop reaches a native Session from when its close has begun."""
    closing = threading.Event()
    go_on = threading.Event()
    detach = _native.Session.detach

    def detached(self: _native.Session) -> Callable[[], None]:
        destroy = detach(self)

        def held() -> None:
            closing.set()
            go_on.wait(WAIT_SECONDS)
            destroy()

        return held

    monkeypatch.setattr(_native.Session, "detach", detached)

    async def main() -> None:
        session = await mitto.AsyncSession().open()
        cookies = session.cookies
        closed = asyncio.create_task(session.close())
        assert await wait_for(closing)
        with pytest.raises(mitto.RequestError) as caught:
            await session.get(server.url("/body"))
        assert caught.value.net_error == ERR_INVALID_ARGUMENT
        with pytest.raises(ValueError, match="has been closed"):
            len(cookies)
        go_on.set()
        await closed

    run(main())


def test_close_ends_what_waits_for_a_response(server: Server) -> None:
    async def main() -> None:
        session = await mitto.AsyncSession().open()
        running = [asyncio.create_task(session.get(server.url("/silent"))) for _ in range(3)]
        await asyncio.sleep(0.05)
        await session.close()
        for task in running:
            with pytest.raises(mitto.RequestError) as caught:
                await task
            assert caught.value.net_error == ERR_ABORTED

    run(main())


def test_close_ends_what_reads_a_body(server: Server) -> None:
    async def main() -> None:
        session = await mitto.AsyncSession().open()
        async with session.stream("GET", server.url("/endless")) as response:
            assert await anext(response.iter_bytes())
            await session.close()
            with pytest.raises(mitto.RequestError) as caught:
                await response.read()
        assert caught.value.net_error == ERR_ABORTED

    run(main())


def test_close_leaves_no_reader_on_the_loop(monkeypatch: pytest.MonkeyPatch, kind_of_loop: str) -> None:
    if event_loops.waits_on_a_thread(kind_of_loop):
        pytest.skip("a thread waits, and the loop watches no file descriptor")
    fds: list[int] = []
    event_fd = _native.Session.event_fd

    def recorded(self: _native.Session) -> int:
        fds.append(event_fd(self))
        return fds[-1]

    monkeypatch.setattr(_native.Session, "event_fd", recorded)

    async def main() -> bool:
        async with mitto.AsyncSession():
            assert fds
        return asyncio.get_running_loop().remove_reader(fds[0])

    assert run(main()) is False


def test_a_close_while_it_opens_waits_for_that_and_closes(held_opening: HeldOpening, server: Server) -> None:
    async def main() -> None:
        session = mitto.AsyncSession()
        opening = asyncio.create_task(session.open())
        await asyncio.sleep(0)
        closing = asyncio.create_task(session.close())
        await asyncio.sleep(0)
        held_opening.go_on.set()
        await closing
        assert await opening is session
        with pytest.raises(mitto.RequestError) as caught:
            await session.get(server.url("/body"))
        assert caught.value.net_error == ERR_INVALID_ARGUMENT
        with pytest.raises(ValueError, match="has been closed"):
            len(session.cookies)

    run(main())


async def _cache_opens(path: Path) -> None:
    """Return once another Session can have the directory, which says that the last one to have it has closed."""
    while True:
        try:
            async with mitto.AsyncSession(cache=path):
                return
        except OSError:
            await asyncio.sleep(0.01)


def test_a_task_cancelled_while_it_opens_leaves_nothing_open(tmp_path: Path, held_opening: HeldOpening) -> None:
    cache = tmp_path / "cache"

    async def enter() -> None:
        async with mitto.AsyncSession(cache=cache):
            pytest.fail("it was cancelled before it opened")

    async def main() -> None:
        entering = asyncio.create_task(enter())
        assert await wait_for(held_opening.started)
        entering.cancel()
        with pytest.raises(asyncio.CancelledError):
            await entering
        await asyncio.to_thread(held_opening.let_go_and_wait)
        await _cache_opens(cache)

    run(main())


@pytest.mark.parametrize("reach", ["cancelled_entering", "still_opening"])
def test_the_end_of_the_loop_while_it_opens_leaves_nothing_open(
    tmp_path: Path, held_opening: HeldOpening, reach: str
) -> None:
    """Ending a loop cancels every task, shielded or not, and the thread that opens a Session finishes after that."""
    cache = tmp_path / "cache"

    async def enter() -> None:
        async with mitto.AsyncSession(cache=cache):
            pytest.fail("it was cancelled before it opened")

    async def main() -> None:
        entering = asyncio.create_task(enter())
        assert await wait_for(held_opening.started)
        if reach == "cancelled_entering":
            entering.cancel()
            with pytest.raises(asyncio.CancelledError):
                await entering

    asyncio.run(main(), loop_factory=event_loops.new_event_loop)
    held_opening.let_go_and_wait()
    run(_cache_opens(cache))


def test_a_cancel_that_comes_just_as_it_opens_leaves_nothing_open(tmp_path: Path, held_opening: HeldOpening) -> None:
    """The thread has handed it over, and the task that was to take it hears of the cancel first."""
    cache = tmp_path / "cache"

    async def main() -> None:
        session = mitto.AsyncSession(cache=cache)
        waiting = asyncio.create_task(session.open())
        assert await wait_for(held_opening.started)
        held_opening.let_go_and_wait()
        # On purpose this holds up the loop, so that no callback tells the task what the thread does in this time.
        time.sleep(0.2)
        opening = session._opening  # pyright: ignore[reportPrivateUsage]
        assert opening is not None
        opening.cancel()
        with pytest.raises(asyncio.CancelledError):
            await waiting
        await _cache_opens(cache)

    run(main())


def test_cancelling_one_first_request_leaves_another_to_open_it(server: Server, held_opening: HeldOpening) -> None:
    async def main() -> bytes:
        session = mitto.AsyncSession()
        first = asyncio.create_task(session.get(server.url("/body")))
        second = asyncio.create_task(session.get(server.url("/body")))
        assert await wait_for(held_opening.started)
        first.cancel()
        with pytest.raises(asyncio.CancelledError):
            await first
        held_opening.go_on.set()
        content = (await second).content
        await session.close()
        return content

    assert run(main()) == BODY


def test_the_end_of_the_loop_during_a_close_leaves_it_closed(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """The end of a loop cancels a task that has only just been made, before it has done anything."""
    cache = tmp_path / "cache"

    async def main() -> None:
        session = await mitto.AsyncSession(cache=cache).open()
        asyncio.create_task(session.close())  # noqa: RUF006
        await asyncio.sleep(0)

    asyncio.run(main(), loop_factory=event_loops.new_event_loop)
    run(_cache_opens(cache))


def test_a_task_cancelled_while_it_closes_leaves_it_closing(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    closing = threading.Event()
    go_on = threading.Event()
    detach = _native.Session.detach
    cache = tmp_path / "cache"

    def detached(self: _native.Session) -> Callable[[], None]:
        destroy = detach(self)

        def held() -> None:
            closing.set()
            go_on.wait(WAIT_SECONDS)
            destroy()

        return held

    monkeypatch.setattr(_native.Session, "detach", detached)

    async def main() -> None:
        session = await mitto.AsyncSession(cache=cache).open()
        closer = asyncio.create_task(session.close())
        assert await wait_for(closing)
        closer.cancel()
        with pytest.raises(asyncio.CancelledError):
            await closer
        go_on.set()
        await session.close()
        await _cache_opens(cache)

    run(main())


def test_a_task_cancelled_while_it_closes_what_still_opens_leaves_it_closing(
    tmp_path: Path, held_opening: HeldOpening
) -> None:
    cache = tmp_path / "cache"

    async def main() -> None:
        session = mitto.AsyncSession(cache=cache)
        opening = asyncio.create_task(session.open())
        assert await wait_for(held_opening.started)
        closer = asyncio.create_task(session.close())
        await asyncio.sleep(0)
        closer.cancel()
        with pytest.raises(asyncio.CancelledError):
            await closer
        await asyncio.to_thread(held_opening.let_go_and_wait)
        await opening
        await _cache_opens(cache)

    run(main())


def test_opening_and_closing_do_not_wait_for_a_thread_that_a_callers_iterable_holds(server: Server) -> None:
    """A caller's iterable may hold every thread of the loop's, and wait for what comes after the end of the Session."""
    waiting = threading.Event()
    go_on = threading.Event()

    def parts() -> Iterator[bytes]:
        waiting.set()
        go_on.wait(WAIT_SECONDS)
        yield b"part"

    async def main() -> None:
        loop = asyncio.get_running_loop()
        loop.set_default_executor(ThreadPoolExecutor(max_workers=1))
        session = await mitto.AsyncSession().open()
        uploading = asyncio.create_task(session.post(server.url(), data=parts(), headers={"Content-Length": "4"}))
        assert await asyncio.get_running_loop().run_in_executor(_OTHER_THREAD, waiting.wait, WAIT_SECONDS)

        async with mitto.AsyncSession() as other:
            assert len(other.cookies) == 0
        await session.close()

        uploading.cancel()
        with pytest.raises(asyncio.CancelledError):
            await uploading
        go_on.set()

    run(main())


# Cancelling a task.


@pytest.mark.parametrize("read", ["read", "iter_bytes"])
def test_a_read_that_was_cancelled_does_not_pass_for_the_end_of_the_body(server: Server, read: str) -> None:
    async def read_it(response: mitto.AsyncResponseStream) -> object:
        return await response.read() if read == "read" else await anext(response.iter_bytes())

    async def read_again(response: mitto.AsyncResponseStream) -> None:
        while await read_it(response):
            pass

    async def main() -> None:
        async with mitto.AsyncSession() as session, session.stream("GET", server.url("/slow-body")) as response:
            with pytest.raises(TimeoutError):
                async with asyncio.timeout(0.05):
                    await read_it(response)
            with pytest.raises(mitto.RequestError) as caught:
                await read_again(response)
            assert caught.value.net_error == ERR_ABORTED

    run(main())
    assert server.client_left.wait(WAIT_SECONDS)


def test_a_second_task_that_reads_one_response_does_not_wait_forever(server: Server) -> None:
    async def reader(response: mitto.AsyncResponseStream) -> bytes:
        return b"".join([chunk async for chunk in response.iter_bytes()])

    async def main() -> list[object]:
        async with mitto.AsyncSession() as session, session.stream("GET", server.url("/held")) as response:
            readers = [asyncio.create_task(reader(response)) for _ in range(2)]
            await asyncio.sleep(0.01)
            server.answer.set()
            return await asyncio.gather(*readers, return_exceptions=True)

    # Which of them gets which part is anybody's guess. A reader that finds the end taken gets a RequestError.
    assert all(isinstance(result, bytes | mitto.RequestError) for result in run(main()))


def test_cancelling_a_task_cancels_the_native_request_that_waits_for_a_response(server: Server) -> None:
    async def main() -> bytes:
        async with mitto.AsyncSession() as session:
            running = asyncio.create_task(session.get(server.url("/silent")))
            assert await wait_for(server.client_arrived)
            running.cancel()
            with pytest.raises(asyncio.CancelledError):
                await running
            assert await wait_for(server.client_left)
            return (await session.get(server.url("/body"))).content

    assert run(main()) == BODY


def test_cancelling_a_task_cancels_the_native_request_that_reads_a_body(server: Server) -> None:
    async def read(session: mitto.AsyncSession) -> None:
        async with session.stream("GET", server.url("/endless")) as response:
            async for _ in response.iter_bytes():
                pass

    async def main() -> None:
        async with mitto.AsyncSession() as session:
            running = asyncio.create_task(read(session))
            await asyncio.sleep(0.05)
            assert not running.done()
            running.cancel()
            with pytest.raises(asyncio.CancelledError):
                await running
            assert await wait_for(server.client_left)

    run(main())


def test_a_timeout_of_asyncio_cancels_the_native_request(server: Server) -> None:
    async def main() -> None:
        async with mitto.AsyncSession() as session:
            with pytest.raises(TimeoutError):
                async with asyncio.timeout(1):
                    await session.get(server.url("/silent"))
            assert await wait_for(server.client_arrived)
            assert await wait_for(server.client_left)

    run(main())


def test_a_timer_of_a_request_raises_what_it_raises_on_a_session(server: Server) -> None:
    async def main() -> None:
        async with mitto.AsyncSession(timeout=0.05) as session:
            with pytest.raises(mitto.RequestTimeoutError):
                await session.get(server.url("/silent"))

    run(main())


def test_cancelling_one_task_leaves_the_others_alone(server: Server) -> None:
    count = 20

    async def main() -> None:
        async with mitto.AsyncSession() as session:
            running = [asyncio.create_task(session.get(server.url(f"/echo/{i}"))) for i in range(count)]
            held = asyncio.create_task(session.get(server.url("/held")))
            await asyncio.sleep(0)
            held.cancel()
            with pytest.raises(asyncio.CancelledError):
                await held
            responses = await asyncio.gather(*running)
            assert [response.content for response in responses] == [f"/echo/{i}".encode() for i in range(count)]

    run(main())


def test_cancelling_what_waits_for_a_callers_iterable_leaves_the_thread_it_waits_on(server: Server) -> None:
    waiting = threading.Event()
    go_on = threading.Event()

    def parts() -> Iterator[bytes]:
        waiting.set()
        go_on.wait(WAIT_SECONDS)
        yield b"part"

    async def main() -> None:
        async with mitto.AsyncSession() as session:
            running = asyncio.create_task(session.post(server.url(), data=parts(), headers={"Content-Length": "100"}))
            assert await wait_for(waiting)
            running.cancel()
            with pytest.raises(asyncio.CancelledError):
                await running
            go_on.set()

    run(main())


def test_cancelling_what_waits_for_a_callers_async_iterable_ends_it_there(server: Server) -> None:
    waiting = asyncio.Event()

    async def parts() -> AsyncIterator[bytes]:
        waiting.set()
        await asyncio.Event().wait()
        yield b"never"

    async def main() -> None:
        async with mitto.AsyncSession() as session:
            running = asyncio.create_task(session.post(server.url(), data=parts(), headers={"Content-Length": "100"}))
            await waiting.wait()
            running.cancel()
            with pytest.raises(asyncio.CancelledError):
                await running

    run(main())


# A stream body.


def test_an_async_iterable_is_a_stream_of_the_size_in_its_headers(server: Server) -> None:
    async def main() -> int:
        async with mitto.AsyncSession() as session:
            response = await session.post(
                server.url("/up"), data=parts_of(*PARTS), headers={"Content-Length": str(len(WHOLE))}
            )
            return response.status

    assert run(main()) == 200
    assert server.received == [("POST", "/up", WHOLE)]


def test_a_part_larger_than_a_write_goes_whole(server: Server) -> None:
    async def main() -> None:
        async with mitto.AsyncSession() as session:
            await session.post(
                server.url("/up"),
                data=parts_of(LARGE_PART, b"end"),
                headers={"Content-Length": str(len(LARGE_PART) + 3)},
            )

    run(main())
    assert server.received[-1][2] == LARGE_PART + b"end"


def test_an_async_iterable_with_no_size_goes_over_http2() -> None:
    case = next(case for case in CASES if case.name == "fetch_post_stream")
    capture_run = run_of(case.name, HTTP2)
    assert capture_run is not None

    async def main(capture: CaptureServer) -> str:
        url = capture.url(capture_run.route, host=capture_run.host, path=case.path)
        async with mitto.AsyncSession(_settings=session_settings(capture, capture_run)) as session:
            return (await session.post(url, data=parts_of(*PARTS))).protocol

    with CaptureServer() as capture:
        assert run(main(capture)) == HTTP2
        form = normal_form(capture.recording(capture_run.route, host=capture_run.host), [case])

    # The normal form shows what goes in each frame, and that is what a stream is about.
    assert cast(Any, form["requests"])[case.name]["body"] == WHOLE.decode()


def test_an_async_iterable_with_no_size_is_refused_over_http1(server: Server) -> None:
    async def main() -> None:
        async with mitto.AsyncSession() as session:
            with pytest.raises(mitto.TLSError) as caught:
                await session.post(server.url("/up"), data=parts_of(*PARTS))
            assert caught.value.net_error == ERR_ALPN_NEGOTIATION_FAILED

    run(main())
    assert not server.received


@pytest.mark.parametrize(
    ("declared", "message"),
    [(len(WHOLE) - 1, "more bytes than its Content-Length"), (len(WHOLE) + 1, "fewer than its Content-Length")],
    ids=["more", "fewer"],
)
def test_an_async_iterable_has_to_keep_to_its_size(server: Server, declared: int, message: str) -> None:
    async def main() -> None:
        async with mitto.AsyncSession() as session:
            with pytest.raises(ValueError, match=message):
                await session.post(server.url("/up"), data=parts_of(*PARTS), headers={"Content-Length": str(declared)})

    run(main())


def test_an_async_iterable_that_gives_no_bytes_raises_a_type_error(server: Server) -> None:
    async def text() -> AsyncIterator[Any]:
        yield "text"

    async def main() -> None:
        async with mitto.AsyncSession() as session:
            with pytest.raises(TypeError, match="has to give bytes"):
                await session.post(server.url("/up"), data=text(), headers={"Content-Length": "4"})

    run(main())


def test_what_an_async_iterable_raises_ends_the_request(server: Server) -> None:
    async def fails() -> AsyncIterator[bytes]:
        yield b"part"
        raise KeyError("from the caller")

    async def main() -> None:
        async with mitto.AsyncSession() as session:
            with pytest.raises(KeyError, match="from the caller"):
                await session.post(server.url("/up"), data=fails(), headers={"Content-Length": "100"})
            assert (await session.get(server.url("/body"))).content == BODY

    run(main())


def test_an_async_iterable_is_refused_beside_another_body_and_with_get(server: Server) -> None:
    async def main() -> None:
        async with mitto.AsyncSession() as session:
            with pytest.raises(ValueError, match="json cannot go together"):
                await session.post(server.url("/up"), data=parts_of(), json=[1])
            with pytest.raises(ValueError, match="beside files"):
                await session.post(server.url("/up"), data=parts_of(), files={"a": b"b"})
            with pytest.raises(ValueError, match="refuses a body with GET"):
                await session.get(server.url("/up"), data=parts_of())

    run(main())
    assert not server.received


def test_a_303_after_an_async_iterable_leads_to_its_page(server: Server) -> None:
    async def main() -> mitto.Response:
        async with mitto.AsyncSession() as session:
            return await session.post(
                server.url("/see-other"), data=parts_of(*PARTS), headers={"Content-Length": str(len(WHOLE))}
            )

    response = run(main())
    assert response.content == BODY
    assert [hop.status for hop in response.history] == [303]


def test_a_307_fails_a_stream_that_cannot_go_again(server: Server) -> None:
    async def main() -> None:
        async with mitto.AsyncSession() as session:
            with pytest.raises(mitto.RequestError) as caught:
                await session.post(
                    server.url("/temporary"), data=parts_of(*PARTS), headers={"Content-Length": str(len(WHOLE))}
                )
            assert caught.value.net_error == ERR_UPLOAD_STREAM_REWIND_NOT_SUPPORTED

    run(main())


def test_a_callers_iterable_and_file_are_read_off_the_loop(server: Server, tmp_path: Path) -> None:
    threads: list[int] = []

    def parts() -> Iterator[bytes]:
        for part in PARTS:
            threads.append(threading.get_ident())
            yield part

    @dataclass
    class WatchedFile:
        data: bytes

        def read(self, size: int) -> bytes:
            threads.append(threading.get_ident())
            taken, self.data = self.data[:size], self.data[size:]
            return taken

    async def main() -> int:
        async with mitto.AsyncSession() as session:
            await session.post(server.url("/up"), data=parts(), headers={"Content-Length": str(len(WHOLE))})
            await session.post(server.url("/up"), data=WatchedFile(WHOLE), headers={"Content-Length": str(len(WHOLE))})
            return threading.get_ident()

    loop_thread = run(main())
    assert server.received[-2:] == [("POST", "/up", WHOLE), ("POST", "/up", WHOLE)]
    assert threads
    assert loop_thread not in threads


def test_a_file_says_its_size_off_the_loop(server: Server, tmp_path: Path) -> None:
    calls: dict[str, int] = {}

    class WatchedFile(io.BytesIO):
        def tell(self) -> int:
            calls["tell"] = threading.get_ident()
            return super().tell()

        def seek(self, offset: int, whence: int = os.SEEK_SET, /) -> int:
            calls["seek"] = threading.get_ident()
            return super().seek(offset, whence)

    async def main() -> int:
        async with mitto.AsyncSession() as session:
            await session.post(server.url("/up"), data=WatchedFile(WHOLE))
            return threading.get_ident()

    loop_thread = run(main())
    assert server.received[-1] == ("POST", "/up", WHOLE)
    assert set(calls) == {"tell", "seek"}
    assert loop_thread not in calls.values()


def test_a_close_that_begins_while_a_file_says_its_size_refuses_the_request(server: Server) -> None:
    measuring = threading.Event()
    go_on = threading.Event()

    class HeldFile(io.BytesIO):
        def tell(self) -> int:
            measuring.set()
            go_on.wait(WAIT_SECONDS)
            return super().tell()

    async def main() -> None:
        session = await mitto.AsyncSession().open()
        posting = asyncio.create_task(session.post(server.url("/up"), data=HeldFile(WHOLE)))
        assert await asyncio.get_running_loop().run_in_executor(_OTHER_THREAD, measuring.wait, WAIT_SECONDS)
        await session.close()
        go_on.set()
        with pytest.raises(mitto.RequestError) as caught:
            await posting
        assert caught.value.net_error == ERR_INVALID_ARGUMENT

    run(main())
    assert not server.received


def test_an_iterable_that_waits_for_a_request_on_the_same_session_does_not_hold_up_the_loop(server: Server) -> None:
    done = threading.Event()

    def parts() -> Iterator[bytes]:
        assert done.wait(WAIT_SECONDS)
        yield b"part"

    async def main() -> None:
        async with mitto.AsyncSession() as session:
            uploading = asyncio.create_task(
                session.post(server.url("/up"), data=parts(), headers={"Content-Length": "4"})
            )
            assert (await session.get(server.url("/body"))).content == BODY
            done.set()
            assert (await uploading).status == 200

    run(main())


# The end of a process.


def _run_python(code: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-c", textwrap.dedent(code)], capture_output=True, text=True, timeout=WAIT_SECONDS * 2
    )


@pytest.mark.parametrize(
    "ending",
    ["pass", "asyncio.create_task(session.get(URL)); await asyncio.sleep(0.05)"],
    ids=["nothing_running", "a_request_running"],
)
def test_a_process_that_never_closes_it_ends_as_it_should(server: Server, ending: str, kind_of_loop: str) -> None:
    ended = _run_python(
        f"""
        import asyncio
        import mitto
        from mitto import _async_api

        URL = {server.url("/silent")!r}
        _async_api._WAIT_ON_A_THREAD = {event_loops.waits_on_a_thread(kind_of_loop)!r}

        async def main():
            session = await mitto.AsyncSession().open()
            {ending}

        asyncio.run(main(), loop_factory=asyncio.{event_loops.loop_class_name(kind_of_loop)})
        print("ended")
        """
    )

    assert (ended.returncode, ended.stdout.strip()) == (0, "ended"), ended.stderr
    assert "Traceback" not in ended.stderr
