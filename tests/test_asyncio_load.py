"""One event loop under load: 1000 requests at once take no thread each, and a large body leaves the loop running.

The client is tests/asyncio_load_client.py, in a process of its own, and this counts its threads from outside. The limits
leave room for a slow CI runner. What the code does here today is far below them, and the pull request has the numbers.
"""

import ctypes
import http.server
import json
import queue
import subprocess
import sys
import threading
import time
from collections.abc import Iterator
from pathlib import Path
from typing import Any, cast
from urllib.parse import parse_qs, urlsplit

import pytest

from mitto._native import find_library
from tests import event_loops
from tests.local_server import QuietServer

_MIB = 1024**2
_CLIENT = Path(__file__).with_name("asyncio_load_client.py")
_RUN_SECONDS = 300
_READ_SIZE = _MIB
_PART = bytes(range(256)) * 256

# A thread per request would be 1000. The default executor of the loop alone may grow to 32, and asks for none here.
_MOST_NEW_THREADS = 32

# How late the loop may run. The body goes in parts of at most 64 KiB, so nothing on the loop should take this long.
_MOST_LAG_SECONDS = 0.5

# What holds each of 1000 requests up at the server, which has six connections to answer them on, so that they stay in the
# native core together for as long as it takes to count threads many times.
_SMALL_ANSWER_SECONDS = 0.005


class _Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        url = urlsplit(self.path)
        if url.path == "/download":
            size = int(parse_qs(url.query)["size"][0])
            self.send_head(size)
            left = size
            while left > 0:
                part = _PART[: min(left, len(_PART))]
                self.wfile.write(part)
                left -= len(part)
        else:
            cast(_Server, self.server).count_answer()
            if url.query:
                time.sleep(_SMALL_ANSWER_SECONDS)
            self.send_head(len(b"small"))
            self.wfile.write(b"small")

    def do_POST(self) -> None:
        left = int(self.headers["Content-Length"])
        received = 0
        while left > 0:
            data = self.rfile.read(min(left, _READ_SIZE))
            assert data
            received += len(data)
            left -= len(data)
        body = str(received).encode()
        self.send_head(len(body))
        self.wfile.write(body)

    def send_head(self, size: int) -> None:
        self.send_response(200)
        self.send_header("Content-Length", str(size))
        self.end_headers()

    def log_message(self, format: str, *args: Any) -> None:
        pass


class _Server(QuietServer):
    request_queue_size = 128

    def __init__(self, address: tuple[str, int], handler: type[_Handler]) -> None:
        super().__init__(address, handler)
        self._lock = threading.Lock()
        self.answers = 0

    def count_answer(self) -> None:
        with self._lock:
            self.answers += 1


@pytest.fixture
def server() -> Iterator[_Server]:
    with _Server(("127.0.0.1", 0), _Handler) as running:
        threading.Thread(target=running.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True).start()
        yield running
        running.shutdown()


def _url_of(server: _Server) -> str:
    return f"http://127.0.0.1:{server.server_port}/"


class _ThreadEntry(ctypes.Structure):
    """THREADENTRY32 of the Windows API."""

    _fields_ = (
        ("dwSize", ctypes.c_uint32),
        ("cntUsage", ctypes.c_uint32),
        ("th32ThreadID", ctypes.c_uint32),
        ("th32OwnerProcessID", ctypes.c_uint32),
        ("tpBasePri", ctypes.c_int32),
        ("tpDeltaPri", ctypes.c_int32),
        ("dwFlags", ctypes.c_uint32),
    )


def _threads_of(pid: int) -> int:
    """Return how many threads a process has now, the operating system's and all."""
    if sys.platform == "win32":
        # Windows lists the threads of every process at once.
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
        kernel32.CreateToolhelp32Snapshot.argtypes = (ctypes.c_uint32, ctypes.c_uint32)
        kernel32.Thread32First.argtypes = (ctypes.c_void_p, ctypes.POINTER(_ThreadEntry))
        kernel32.Thread32Next.argtypes = (ctypes.c_void_p, ctypes.POINTER(_ThreadEntry))
        kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
        th32cs_snapthread = 0x4
        snapshot = kernel32.CreateToolhelp32Snapshot(th32cs_snapthread, 0)
        if snapshot in (None, ctypes.c_void_p(-1).value):
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            entry = _ThreadEntry(dwSize=ctypes.sizeof(_ThreadEntry))
            count = 0
            found = kernel32.Thread32First(snapshot, ctypes.byref(entry))
            while found:
                count += entry.th32OwnerProcessID == pid
                found = kernel32.Thread32Next(snapshot, ctypes.byref(entry))
            return count
        finally:
            kernel32.CloseHandle(snapshot)
    if sys.platform == "linux":
        return sum(1 for _ in Path(f"/proc/{pid}/task").iterdir())
    listing = subprocess.run(["ps", "-M", "-p", str(pid)], capture_output=True, text=True, check=True).stdout
    return len(listing.splitlines()) - 1


def _lines_of(process: subprocess.Popen[str]) -> "queue.Queue[str]":
    lines: queue.Queue[str] = queue.Queue()

    def read() -> None:
        assert process.stdout is not None
        for line in process.stdout:
            lines.put(line)

    threading.Thread(target=read, daemon=True).start()
    return lines


@pytest.fixture(params=event_loops.KINDS)
def kind_of_loop(request: pytest.FixtureRequest) -> str:
    """The way that the client's event loop hears of its Session's events, each in turn."""
    return request.param


def run_load(load: str, url: str, size: int, kind_of_loop: str) -> dict[str, Any]:
    """Run the client, and return what it printed, with the threads it had before the load and at most during it."""
    if find_library() is None:
        pytest.skip("no native library")
    wait_on_a_thread = ["--wait-on-a-thread"] if event_loops.waits_on_a_thread(kind_of_loop) else []
    arguments = [load, url, str(size), event_loops.loop_class_name(kind_of_loop), *wait_on_a_thread]
    with subprocess.Popen(
        [sys.executable, str(_CLIENT), *arguments], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True
    ) as process:
        assert process.stdin is not None
        lines = _lines_of(process)
        try:
            ready, pid = lines.get(timeout=_RUN_SECONDS).split()
            assert ready == "ready"
            client = int(pid)
            threads_before = _threads_of(client)
            process.stdin.write("go\n")
            process.stdin.flush()
            most_threads = threads_before
            deadline = time.monotonic() + _RUN_SECONDS
            while lines.empty():
                assert time.monotonic() < deadline
                # A process that has ended before its line got here has no threads, and no list of them.
                if process.poll() is None:
                    most_threads = max(most_threads, _threads_of(client))
                time.sleep(0.01)
            result = json.loads(lines.get())
        finally:
            process.kill()
    return {**result, "threads_before": threads_before, "most_threads": most_threads}


def test_a_thousand_requests_on_one_loop_take_no_thread_each(server: _Server, kind_of_loop: str) -> None:
    result = run_load("concurrent", _url_of(server), 1000, kind_of_loop)
    print(result)

    assert result["ok"] == 1000
    assert result["right"] == 1000
    # And the one that the client made to be ready.
    assert server.answers == 1001
    assert result["most_threads"] - result["threads_before"] <= _MOST_NEW_THREADS
    assert result["lag_max"] < _MOST_LAG_SECONDS


@pytest.mark.parametrize(
    ("load", "size"),
    [
        ("stream_download", 1024 * _MIB),
        ("whole_download", 256 * _MIB),
        ("iterable_upload", 256 * _MIB),
        ("async_iterable_upload", 256 * _MIB),
    ],
    ids=["stream_download_1GiB", "whole_download_256MiB", "iterable_upload_256MiB", "async_iterable_upload_256MiB"],
)
def test_the_loop_stays_responsive_during_a_large_body(
    server: _Server, load: str, size: int, kind_of_loop: str
) -> None:
    result = run_load(load, _url_of(server), size, kind_of_loop)
    print(result)

    assert result["received"] == size
    assert result["lag_max"] < _MOST_LAG_SECONDS
    assert result["small_requests"] > 0
    assert result["small_max"] < _MOST_LAG_SECONDS * 2
