"""Checks the channel prototype from Python: a Page's events reach asyncio, and
a Page ends when the Python process that opened it dies without closing it.

Nothing of the prototype merges. Usage, on macOS or Linux:

    python3 tools/page_channel_prototype.py <libmitto_core> <mitto_page_prototype>
"""

import asyncio
import ctypes
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

SETTINGS = json.dumps({"version": 1}).encode()

PAGE_EVENT_READY = 1
PAGE_EVENT_RESULT = 2
PAGE_EVENT_GONE = 5

# How long a Page may outlive the Python process that opened it.
DEATH_TIMEOUT_SECONDS = 5.0


def load(library: Path) -> ctypes.CDLL:
    lib = ctypes.CDLL(str(library))
    pointer = ctypes.c_void_p
    lib.mitto_session_create.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(pointer)]
    lib.mitto_session_event_fd.argtypes = [pointer, ctypes.POINTER(ctypes.c_int32)]
    lib.mitto_session_next_ready_request.argtypes = [pointer, ctypes.POINTER(pointer)]
    lib.mitto_page_open.argtypes = [pointer, ctypes.c_char_p, ctypes.POINTER(pointer)]
    lib.mitto_page_pid.argtypes = [pointer]
    lib.mitto_page_pid.restype = ctypes.c_int64
    lib.mitto_page_evaluate.argtypes = [pointer, ctypes.c_char_p, ctypes.c_size_t]
    lib.mitto_page_next_event.argtypes = [pointer, ctypes.POINTER(ctypes.c_int32), ctypes.POINTER(ctypes.c_char_p)]
    lib.mitto_page_text_free.argtypes = [ctypes.c_char_p]
    return lib


class Page:
    """A Page of the prototype, whose events an asyncio loop waits for."""

    def __init__(self, lib: ctypes.CDLL, program: Path) -> None:
        self._lib = lib
        self._session = ctypes.c_void_p()
        if lib.mitto_session_create(SETTINGS, len(SETTINGS), ctypes.byref(self._session)) != 0:
            raise RuntimeError("no Session")
        fd = ctypes.c_int32()
        if lib.mitto_session_event_fd(self._session, ctypes.byref(fd)) != 0:
            raise RuntimeError("no descriptor")
        self.fd = fd.value
        self._page = ctypes.c_void_p()
        if lib.mitto_page_open(self._session, str(program).encode(), ctypes.byref(self._page)) != 0:
            raise RuntimeError("no Page")
        self.pid: int = lib.mitto_page_pid(self._page)

    def evaluate(self, script: str) -> None:
        data = script.encode()
        self._lib.mitto_page_evaluate(self._page, data, len(data))

    async def next_event(self) -> tuple[int, str]:
        """Waits on the Session's descriptor with the loop, as the asyncio Session would."""
        loop = asyncio.get_running_loop()
        while True:
            event = self._take_event()
            if event is not None:
                return event
            readable = loop.create_future()
            loop.add_reader(self.fd, readable.set_result, None)
            try:
                await readable
            finally:
                loop.remove_reader(self.fd)
            ready = ctypes.c_void_p()
            while self._lib.mitto_session_next_ready_request(self._session, ctypes.byref(ready)) == 0:
                pass

    def _take_event(self) -> tuple[int, str] | None:
        kind = ctypes.c_int32()
        text = ctypes.c_char_p()
        if self._lib.mitto_page_next_event(self._page, ctypes.byref(kind), ctypes.byref(text)) != 0:
            return None
        value = (text.value or b"").decode()
        self._lib.mitto_page_text_free(text)
        return kind.value, value


async def open_and_wait(lib: ctypes.CDLL, program: Path) -> Page:
    page = Page(lib, program)
    kind, _ = await page.next_event()
    assert kind == PAGE_EVENT_READY, kind
    page.evaluate("document.title")
    kind, text = await page.next_event()
    assert kind == PAGE_EVENT_RESULT, kind
    print(f"  asyncio got: {text}", flush=True)
    return page


async def killed_page_reaches_asyncio(lib: ctypes.CDLL, program: Path) -> None:
    page = await open_and_wait(lib, program)
    started = time.perf_counter()
    os.kill(page.pid, signal.SIGKILL)
    kind, text = await page.next_event()
    assert kind == PAGE_EVENT_GONE, kind
    print(f"  asyncio heard after {(time.perf_counter() - started) * 1000:.1f} ms: {text}", flush=True)


def child(library: Path, program: Path, hold_in_fork: bool) -> None:
    """Opens a Page, says its pid, and waits to be killed without closing it."""
    lib = load(library)
    page = asyncio.run(open_and_wait(lib, program))
    if hold_in_fork and os.fork() == 0:
        # This process holds the library's end of the channel too, so the Page
        # reads no end of stream when its parent dies.
        time.sleep(60)
        os._exit(0)
    print(f"page {page.pid}", flush=True)
    time.sleep(60)


def process_exists(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


def page_ends_with_python(library: Path, program: Path, hold_in_fork: bool) -> bool:
    arguments = [sys.executable, __file__, str(library), str(program), "--child"]
    if hold_in_fork:
        arguments.append("--hold-in-fork")
    python = subprocess.Popen(arguments, stdout=subprocess.PIPE, text=True)
    assert python.stdout is not None
    page_pid = 0
    for line in python.stdout:
        print(line, end="")
        if line.startswith("page "):
            page_pid = int(line.split()[1])
            break
    started = time.perf_counter()
    python.send_signal(signal.SIGKILL)
    python.wait()
    while process_exists(page_pid) and time.perf_counter() - started < DEATH_TIMEOUT_SECONDS:
        time.sleep(0.001)
    gone = not process_exists(page_pid)
    print(
        f"  page {page_pid} {'gone' if gone else 'STILL THERE'} {(time.perf_counter() - started) * 1000:.1f} ms after",
        flush=True,
    )
    if hold_in_fork:
        subprocess.run(["pkill", "-KILL", "-f", f"{Path(__file__).name} .* --hold-in-fork"], check=False)
    return gone


def main() -> int:
    library, program = Path(sys.argv[1]), Path(sys.argv[2])
    if "--child" in sys.argv:
        child(library, program, "--hold-in-fork" in sys.argv)
        return 0
    print("A killed Page reaches asyncio through the Session's descriptor")
    asyncio.run(killed_page_reaches_asyncio(load(library), program))
    print("A Page ends when Python dies without closing it")
    ok = page_ends_with_python(library, program, hold_in_fork=False)
    print("... also when a fork of Python still holds the channel's end")
    ok = page_ends_with_python(library, program, hold_in_fork=True) and ok
    print("all passed" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
