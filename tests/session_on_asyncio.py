"""mitto.Session's blocking methods, each of which runs what mitto.AsyncSession has of that name on an event loop.

This is how one feature test can go through both APIs: the both_apis fixture in tests/conftest.py puts it in the place of
mitto.Session. Each one has an event loop of its own in a thread of its own, as a program using asyncio does, and every call
on an AsyncSession and what it returns happens on that thread.
"""

import asyncio
import threading
from collections.abc import Callable, Coroutine, Generator, Iterator
from contextlib import contextmanager, suppress
from types import TracebackType
from typing import Any, Self

import mitto

__all__ = ["SessionOnAsyncio"]

type _Run = Callable[[Coroutine[Any, Any, Any]], Any]


class _BlockingCookies:
    """What AsyncCookies has, called on the event loop."""

    def __init__(self, cookies: mitto.AsyncCookies, run: _Run) -> None:
        self._cookies = cookies
        self._run = run

    def __iter__(self) -> Iterator[mitto.Cookie]:
        return iter(self._on_loop(list, self._cookies))

    def __len__(self) -> int:
        return self._on_loop(len, self._cookies)

    def _on_loop(self, call: Callable[..., Any], *args: Any, **kwargs: Any) -> Any:
        async def call_it() -> Any:
            return call(*args, **kwargs)

        return self._run(call_it())

    def get(self, name: str, **where: Any) -> mitto.Cookie | None:
        return self._on_loop(self._cookies.get, name, **where)

    def set(self, cookie: mitto.Cookie) -> None:
        self._on_loop(self._cookies.set, cookie)

    def delete(self, name: str, **where: Any) -> int:
        return self._on_loop(self._cookies.delete, name, **where)

    def clear(self) -> int:
        return self._on_loop(self._cookies.clear)

    def save(self, path: Any) -> None:
        self._run(self._cookies.save(path))

    def load(self, path: Any) -> None:
        self._run(self._cookies.load(path))


class _BlockingChunks:
    def __init__(self, chunks: Any, run: _Run) -> None:
        self._chunks = chunks
        self._run = run

    def __iter__(self) -> Self:
        return self

    def __next__(self) -> bytes:
        async def next_chunk() -> bytes | None:
            return await anext(self._chunks, None)

        chunk = self._run(next_chunk())
        if chunk is None:
            raise StopIteration
        return chunk


class _BlockingStream:
    """What AsyncResponseStream has, read from the event loop."""

    def __init__(self, stream: mitto.AsyncResponseStream, run: _Run) -> None:
        self._stream = stream
        self._run = run
        self.status = stream.status
        self.url = stream.url
        self.protocol = stream.protocol
        self.headers = stream.headers
        self.history = stream.history
        self.from_cache = stream.from_cache

    def iter_bytes(self) -> Iterator[bytes]:
        return _BlockingChunks(self._stream.iter_bytes(), self._run)

    def read(self) -> bytes:
        return self._run(self._stream.read())


class SessionOnAsyncio:
    """The methods of mitto.Session, on a mitto.AsyncSession on an event loop of this object's own."""

    def __init__(self, on_create: Callable[[Self], None], **settings: Any) -> None:
        self._loop = asyncio.new_event_loop()
        self._thread = threading.Thread(target=self._loop.run_forever, daemon=True, name="session-on-asyncio")
        self._thread.start()
        self._session = mitto.AsyncSession(**settings)
        on_create(self)
        self._run(self._session.open())
        self.cookies = _BlockingCookies(self._session.cookies, self._run)
        self._native = self._session._opening.result().state.native  # pyright: ignore[reportPrivateUsage, reportOptionalMemberAccess]

    @property
    def machine(self) -> mitto.Machine:
        return self._session.machine

    def __enter__(self) -> Self:
        return self

    def __exit__(
        self, exc_type: type[BaseException] | None, exc: BaseException | None, traceback: TracebackType | None
    ) -> None:
        self.close()

    def _run(self, coroutine: Coroutine[Any, Any, Any]) -> Any:
        return asyncio.run_coroutine_threadsafe(coroutine, self._loop).result()

    def close(self) -> None:
        self._run(self._session.close())

    def end_of_test(self) -> None:
        """Close the AsyncSession, if the test did not, and end its event loop, which every later call would wait for."""
        # A test that makes the file of cookies fail has seen this already.
        with suppress(OSError):
            self._run(self._session.close())
        self._loop.call_soon_threadsafe(self._loop.stop)
        self._thread.join()
        self._loop.close()

    def request(self, method: str, url: str, **options: Any) -> mitto.Response:
        return self._run(self._session.request(method, url, **options))

    @contextmanager
    def stream(self, method: str, url: str, **options: Any) -> Generator[_BlockingStream]:
        context = self._session.stream(method, url, **options)
        stream = self._run(context.__aenter__())
        try:
            yield _BlockingStream(stream, self._run)
        except BaseException as error:
            if not self._run(context.__aexit__(type(error), error, error.__traceback__)):
                raise
        else:
            self._run(context.__aexit__(None, None, None))

    def get(self, url: str, **options: Any) -> mitto.Response:
        return self._run(self._session.get(url, **options))

    def post(self, url: str, **options: Any) -> mitto.Response:
        return self._run(self._session.post(url, **options))

    def put(self, url: str, **options: Any) -> mitto.Response:
        return self._run(self._session.put(url, **options))

    def patch(self, url: str, **options: Any) -> mitto.Response:
        return self._run(self._session.patch(url, **options))

    def delete(self, url: str, **options: Any) -> mitto.Response:
        return self._run(self._session.delete(url, **options))

    def head(self, url: str, **options: Any) -> mitto.Response:
        return self._run(self._session.head(url, **options))

    def options(self, url: str, **options: Any) -> mitto.Response:
        return self._run(self._session.options(url, **options))
