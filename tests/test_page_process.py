"""Tests of a Page's process and its channel, through the C API, with the native core's C test program.

The library starts the Page program from the directory it was loaded from, so each test copies the test program, the
library beside it and the Page program into a directory of its own. They run where MITTO_CORE_TEST names the test
program and MITTO_PAGE_PROGRAM names a Page program built from the same tree: in CI, on every Host, the test program
of the native job's build and the Page program in the wheel that it installs.
"""

import ctypes
import http.server
import os
import re
import shutil
import signal
import subprocess
import sys
import textwrap
import threading
import time
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest

from mitto import _native
from tests.local_server import LocalHTTPServer

_CORE_TEST_ENV_VAR = "MITTO_CORE_TEST"
_PAGE_PROGRAM_ENV_VAR = "MITTO_PAGE_PROGRAM"
_TIMEOUT_SECONDS = 60
# How long a Page process may outlive the process that holds the library.
_GONE_TIMEOUT_SECONDS = 10
_SETTINGS = '{"version": 1}'
_PAGE_PROGRAM_NAME = "mitto_page.exe" if sys.platform == "win32" else "mitto_page"
_BODY = b"a body"


class _Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        self.send_response(200)
        self.send_header("Content-Length", str(len(_BODY)))
        self.end_headers()
        self.wfile.write(_BODY)

    def log_message(self, format: str, *args: Any) -> None:
        pass


@pytest.fixture
def server_url() -> Iterator[str]:
    """Run a local HTTP server and return its URL."""
    with LocalHTTPServer(("127.0.0.1", 0), _Handler) as server:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        yield f"http://127.0.0.1:{server.server_port}/"
        server.shutdown()


def _named(env_var: str) -> Path:
    path = os.environ.get(env_var)
    if not path:
        pytest.skip(f"set {_CORE_TEST_ENV_VAR} and {_PAGE_PROGRAM_ENV_VAR} to run this test")
    return Path(path)


def _copy_test_program(directory: Path) -> Path:
    """Copy the test program and the library beside it into `directory`, and return the copy of the program."""
    program = _named(_CORE_TEST_ENV_VAR)
    _named(_PAGE_PROGRAM_ENV_VAR)
    directory.mkdir()
    shutil.copy2(program.with_name(_native.LIBRARY_NAME), directory)
    return Path(shutil.copy2(program, directory))


@pytest.fixture
def program(tmp_path: Path) -> Path:
    """Return the test program, in a directory with the library and the Page program."""
    copy = _copy_test_program(tmp_path / "with-page")
    shutil.copy2(_named(_PAGE_PROGRAM_ENV_VAR), copy.with_name(_PAGE_PROGRAM_NAME))
    return copy


def _run(program: Path, scenario: str, url: str = "http://127.0.0.1:1/") -> str:
    result = subprocess.run(
        [program, url, _SETTINGS, scenario], capture_output=True, text=True, timeout=_TIMEOUT_SECONDS, check=False
    )
    assert result.returncode == 0, result.stderr
    return result.stdout


def _process_is_gone(process_id: int) -> bool:
    if sys.platform == "win32":
        synchronize = 0x00100000
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenProcess.restype = ctypes.c_void_p
        handle = kernel32.OpenProcess(synchronize, False, process_id)
        if not handle:
            return True
        try:
            # WAIT_OBJECT_0: it has ended, and only a handle keeps it.
            return kernel32.WaitForSingleObject(ctypes.c_void_p(handle), 0) == 0
        finally:
            kernel32.CloseHandle(ctypes.c_void_p(handle))
    try:
        os.kill(process_id, 0)
    except ProcessLookupError:
        return True
    return False


def _wait_until_gone(process_id: int) -> float:
    """Wait for the process to go, and return how long that took."""
    start = time.perf_counter()
    while not _process_is_gone(process_id):
        assert time.perf_counter() - start < _GONE_TIMEOUT_SECONDS, f"process {process_id} is still there"
        time.sleep(0.01)
    return time.perf_counter() - start


def _read_until(holder: subprocess.Popen[str], prefix: str) -> str:
    assert holder.stdout is not None
    while line := holder.stdout.readline():
        if line.startswith(prefix):
            return line.removeprefix(prefix).strip()
    holder.wait(_TIMEOUT_SECONDS)
    assert holder.stderr is not None
    pytest.fail(f"the test program ended without {prefix!r}: {holder.stderr.read()}")


def _hold(program: Path, scenario: str = "page-hold") -> subprocess.Popen[str]:
    return subprocess.Popen(
        [program, "http://127.0.0.1:1/", _SETTINGS, scenario],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def test_a_page_runs_a_script_and_its_process_goes_when_it_closes(program: Path) -> None:
    assert _run(program, "page") == "evaluated 2\nthe Page process is gone\n"


def test_a_killed_page_fails_each_call_and_its_session_goes_on(program: Path, server_url: str) -> None:
    output = _run(program, "page-kill", server_url)

    # TerminateProcess() on Windows gives the code that the test program chose.
    how = "exited with code 9" if sys.platform == "win32" else f"was ended by signal {signal.SIGKILL.value}"
    lines = output.splitlines()
    assert lines[0] == "evaluated 2"
    # A call in progress when the process ended, and one after.
    assert lines.count(f"failed, process ended: the Page process {how}") == 2
    assert lines.count(f"ended, process ended: the Page process {how}") == 1
    assert "the Page process is gone\nstatus 200 over http/1.1\n" in output
    assert output.endswith(f"body: {len(_BODY)} bytes\n")


def test_destroying_the_session_ends_its_page(program: Path) -> None:
    assert _run(program, "page-destroy-session") == (
        "evaluated 2\n"
        "ended, session destroyed: the Session was destroyed\n"
        "failed, session destroyed: the Session was destroyed\n"
        "closed\n"
    )


def test_the_page_process_ends_when_the_process_that_holds_the_library_is_killed(program: Path) -> None:
    holder = _hold(program)
    try:
        page_process = int(_read_until(holder, "page process "))
        assert not _process_is_gone(page_process)
    finally:
        holder.kill()
        holder.wait(_TIMEOUT_SECONDS)

    _wait_until_gone(page_process)


@pytest.mark.skipif(sys.platform == "win32", reason="Windows has no fork()")
def test_the_page_process_ends_with_its_parent_while_a_fork_holds_the_channel(program: Path) -> None:
    # The fork holds the parent's end of the channel, so the Page reads no end of the stream: the kernel on Linux and
    # the Page's watch on its parent on macOS end it.
    holder = _hold(program, "page-hold-forked")
    fork = 0
    try:
        fork = int(_read_until(holder, "fork "))
        page_process = int(_read_until(holder, "page process "))
    finally:
        holder.kill()
        holder.wait(_TIMEOUT_SECONDS)
    try:
        _wait_until_gone(page_process)
    finally:
        if fork:
            os.kill(fork, signal.SIGKILL)


@pytest.mark.skipif(not sys.platform.startswith("linux"), reason="reads /proc")
def test_the_page_process_holds_no_file_or_socket_beyond_its_channel_on_linux(program: Path) -> None:
    holder = _hold(program)
    try:
        page_process = int(_read_until(holder, "page process "))
        held = {int(fd.name): str(fd.readlink()) for fd in Path(f"/proc/{page_process}/fd").iterdir()}
        started_with = {fd: str(Path(f"/proc/{holder.pid}/fd/{fd}").readlink()) for fd in (1, 2)}
    finally:
        holder.kill()
        holder.wait(_TIMEOUT_SECONDS)

    # Standard input is /dev/null, and standard output and error are the test program's.
    assert held.pop(0) == "/dev/null"
    assert {fd: held.pop(fd) for fd in (1, 2)} == started_with
    # The channel, which the library put at descriptor 3.
    assert re.fullmatch(r"socket:\[\d+\]", held.pop(3))
    # What the Page program made for itself: its message pumps' epoll and eventfd, which name no file.
    assert all(target.startswith("anon_inode:") for target in held.values()), held


_FAKE_PAGE = textwrap.dedent(
    """\
    #!{python}
    # Stands in for the Page program: speaks to the library over its channel as {name} says.
    import socket
    import struct
    import sys

    channel = socket.socket(fileno=int(sys.argv[1].removeprefix("--session=").split(",")[0]))

    def send(message_type, payload):
        channel.sendall(struct.pack("<II", len(payload), message_type) + payload)

    {script}
    # The library ends this process, or closes the channel.
    while channel.recv(65536):
        pass
    """
)


def _fake_page_program(tmp_path: Path, name: str, script: str) -> Path:
    copy = _copy_test_program(tmp_path / name)
    page = copy.with_name(_PAGE_PROGRAM_NAME)
    page.write_text(_FAKE_PAGE.format(python=sys.executable, name=name, script=script))
    page.chmod(0o755)
    return copy


_HELLO = "send(1, struct.pack('<I', 1))"


@pytest.mark.skipif(sys.platform == "win32", reason="the fake Page program is a script")
@pytest.mark.parametrize(
    ("name", "script", "why"),
    [
        pytest.param(
            "another-version",
            "send(1, struct.pack('<I', 99))",
            "the library refused a message from the Page: the Page speaks version 99 of the messages, and the library"
            " speaks 1",
            id="another-version",
        ),
        pytest.param(
            "unknown-message",
            f"{_HELLO}\nsend(77, b'')",
            "the library refused a message from the Page: message type 77 is unknown",
            id="unknown-message",
        ),
        pytest.param(
            "before-hello",
            "send(5, struct.pack('<QB', 1, 0) + b'2')",
            "the library refused a message from the Page: message type 5 came before the hello",
            id="before-hello",
        ),
        pytest.param(
            "unasked-result",
            f"{_HELLO}\nsend(5, struct.pack('<QB', 9, 0) + b'2')",
            "the library refused a message from the Page: a result of call 9, which is not in progress",
            id="unasked-result",
        ),
        pytest.param(
            "too-large",
            f"{_HELLO}\nchannel.sendall(struct.pack('<II', 16 * 1024 * 1024 + 1, 5))",
            "the library refused a message from the Page: a frame of 16777217 bytes is larger than the channel carries",
            id="too-large",
        ),
        pytest.param(
            "page-refuses",
            f"{_HELLO}\nsend(2, 'it does not like you'.encode())",
            "the Page refused a message from the library: it does not like you",
            id="page-refuses",
        ),
    ],
)
def test_a_message_that_one_side_does_not_take_ends_the_page_with_an_error_that_says_so(
    tmp_path: Path, name: str, script: str, why: str
) -> None:
    assert _run(_fake_page_program(tmp_path, name, script), "page-first-call") == (
        f"failed, refused: {why}\nended, refused: {why}\nthe Page process is gone\n"
    )


@pytest.mark.skipif(sys.platform == "win32", reason="the fake Page program is a script")
def test_a_page_that_ends_without_a_word_fails_its_calls(tmp_path: Path) -> None:
    program = _fake_page_program(tmp_path, "exits", f"{_HELLO}\nsys.exit(5)")

    assert _run(program, "page-first-call") == (
        "failed, process ended: the Page process exited with code 5\n"
        "ended, process ended: the Page process exited with code 5\n"
        "the Page process is gone\n"
    )


def test_a_library_with_no_page_program_beside_it_refuses_to_open_a_page(tmp_path: Path) -> None:
    program = _copy_test_program(tmp_path / "no-page")

    result = subprocess.run(
        [program, "http://127.0.0.1:1/", _SETTINGS, "page"],
        capture_output=True,
        text=True,
        timeout=_TIMEOUT_SECONDS,
        check=False,
    )

    assert result.returncode == 1
    assert result.stderr == "opening a Page failed with net error -6\n"
