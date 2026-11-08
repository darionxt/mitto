"""Puts one event loop under a load through mitto.AsyncSession, and prints how long the loop was kept from running.

A process of its own, with the server in another, so that what holds up the loop here is only this client, and what counts
its threads sees only this client's. tests/test_asyncio_load.py runs this.

It opens the Session and makes one request, says "ready" with its process ID and waits for a line, so that the threads it
has by then can be counted first. On Windows the python.exe of a virtual environment starts the interpreter in a process of
its own, so the ID is not the one its parent started. Then it prints one line of JSON. Its last argument names the way the loop hears of the Session's events, as
tests/event_loops.py does.
"""

import argparse
import asyncio
import json
import os
import statistics
import sys
import threading
import time
from collections.abc import AsyncIterator, Awaitable, Callable, Iterator

import mitto
from mitto import _async_api

# How long the heartbeat sleeps, which is how finely it can tell.
_TICK_SECONDS = 0.001

# How often a small request goes out during a large body.
_SMALL_REQUEST_EVERY_SECONDS = 0.05

_PART = bytes(range(256)) * 256


def _parts(size: int) -> Iterator[bytes]:
    left = size
    while left > 0:
        part = _PART[: min(left, len(_PART))]
        left -= len(part)
        yield part


async def _async_parts(size: int) -> AsyncIterator[bytes]:
    for part in _parts(size):
        yield part


class _Heartbeat:
    """Sleeps a moment over and over, and keeps how much later than asked each sleep ended, and the threads of Python."""

    def __init__(self) -> None:
        self.lags: list[float] = []
        self.most_python_threads = threading.active_count()

    async def run(self) -> None:
        loop = asyncio.get_running_loop()
        while True:
            before = loop.time()
            await asyncio.sleep(_TICK_SECONDS)
            self.lags.append(loop.time() - before - _TICK_SECONDS)
            self.most_python_threads = max(self.most_python_threads, threading.active_count())


async def _small_requests(session: mitto.AsyncSession, url: str, seconds: list[float]) -> None:
    """Keep making a small request on the same Session, and keep how long each took."""
    while True:
        before = time.perf_counter()
        await session.get(f"{url}small")
        seconds.append(time.perf_counter() - before)
        await asyncio.sleep(_SMALL_REQUEST_EVERY_SECONDS)


async def _concurrent(session: mitto.AsyncSession, url: str, size: int) -> dict[str, object]:
    responses = await asyncio.gather(*(session.get(f"{url}small?{i}") for i in range(size)))
    return {
        "ok": sum(response.status == 200 for response in responses),
        "right": sum(response.content == b"small" for response in responses),
    }


async def _stream_download(session: mitto.AsyncSession, url: str, size: int) -> dict[str, object]:
    received = 0
    async with session.stream("GET", f"{url}download?size={size}") as response:
        async for chunk in response.iter_bytes():
            received += len(chunk)
    return {"received": received}


async def _whole_download(session: mitto.AsyncSession, url: str, size: int) -> dict[str, object]:
    response = await session.get(f"{url}download?size={size}")
    return {"received": len(response.content)}


async def _iterable_upload(session: mitto.AsyncSession, url: str, size: int) -> dict[str, object]:
    response = await session.post(f"{url}upload", data=_parts(size), headers={"Content-Length": str(size)})
    return {"received": int(response.content)}


async def _async_iterable_upload(session: mitto.AsyncSession, url: str, size: int) -> dict[str, object]:
    response = await session.post(f"{url}upload", data=_async_parts(size), headers={"Content-Length": str(size)})
    return {"received": int(response.content)}


_LOADS: dict[str, Callable[[mitto.AsyncSession, str, int], Awaitable[dict[str, object]]]] = {
    "concurrent": _concurrent,
    "stream_download": _stream_download,
    "whole_download": _whole_download,
    "iterable_upload": _iterable_upload,
    "async_iterable_upload": _async_iterable_upload,
}


async def _main(load: str, url: str, size: int) -> dict[str, object]:
    loop = asyncio.get_running_loop()
    async with mitto.AsyncSession() as session:
        await session.get(f"{url}small")
        print(f"ready {os.getpid()}", flush=True)
        await loop.run_in_executor(None, sys.stdin.readline)

        heartbeat = _Heartbeat()
        beating = asyncio.create_task(heartbeat.run())
        small_seconds: list[float] = []
        # 1000 requests are their own small requests.
        small = None if load == "concurrent" else asyncio.create_task(_small_requests(session, url, small_seconds))
        before = time.perf_counter()
        result = await _LOADS[load](session, url, size)
        seconds = time.perf_counter() - before
        beating.cancel()
        if small is not None:
            small.cancel()

    lags = sorted(heartbeat.lags)
    return {
        **result,
        "seconds": seconds,
        "beats": len(lags),
        "lag_median": statistics.median(lags),
        "lag_p99": lags[int(len(lags) * 0.99)],
        "lag_max": lags[-1],
        "most_python_threads": heartbeat.most_python_threads,
        "small_requests": len(small_seconds),
        "small_median": statistics.median(small_seconds) if small_seconds else None,
        "small_max": max(small_seconds, default=None),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("load", choices=sorted(_LOADS))
    parser.add_argument("url")
    parser.add_argument("size", type=int)
    parser.add_argument("loop_class", help="the name of the class of event loop in asyncio")
    parser.add_argument("--wait-on-a-thread", action="store_true")
    args = parser.parse_args()
    _async_api._WAIT_ON_A_THREAD = args.wait_on_a_thread  # pyright: ignore[reportPrivateUsage]
    loop_factory = getattr(asyncio, args.loop_class)
    print(json.dumps(asyncio.run(_main(args.load, args.url, args.size), loop_factory=loop_factory)), flush=True)


if __name__ == "__main__":
    main()
