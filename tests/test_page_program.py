"""Tests of the Page program, mitto_page, which runs the script on its command line on V8 and prints what it returns.

These run where MITTO_PAGE_PROGRAM names a program built with tools/workspace/runtime_args.gn: in CI, the one installed
from the wheel that the native job builds, and the Sanitizers workflow's builds of it with ASan and UBSan. What a page
sees is compared with Chrome elsewhere: these check what holds of any Chrome, such as compilers that give a page
WebAssembly, and ICU's whole data behind Intl.

The program also serves a Page for the library, over the channel that its --session= argument names. The tests of that
here stand in for the library on macOS and Linux, where the channel is a socket, so that they run under the sanitizers,
which cannot load the library into Python. tests/test_page_process.py tests the two together on every Host.
"""

import json
import os
import re
import socket
import struct
import subprocess
import sys
from collections.abc import Iterator
from pathlib import Path

import pytest

_PROGRAM_ENV_VAR = "MITTO_PAGE_PROGRAM"
_TIMEOUT_SECONDS = 60
_SCRIPT_THREW = 1
_WRONG_ARGUMENTS = 2


@pytest.fixture
def program() -> Path:
    """Return the Page program, or skip if the environment names none."""
    path = os.environ.get(_PROGRAM_ENV_VAR)
    if not path:
        pytest.skip(f"no Page program; set {_PROGRAM_ENV_VAR} to run this test")
    return Path(path)


def _run(program: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [program, *arguments], capture_output=True, encoding="utf-8", timeout=_TIMEOUT_SECONDS, check=False
    )


def _evaluate(program: Path, script: str) -> str:
    result = _run(program, script)
    assert (result.returncode, result.stderr) == (0, "")
    return result.stdout.removesuffix("\n")


def test_it_prints_the_version_of_v8(program: Path) -> None:
    result = _run(program, "--version")

    assert result.returncode == 0
    assert re.fullmatch(r"\d+\.\d+\.\d+\.\d+\n", result.stdout)


@pytest.mark.parametrize(
    ("script", "printed"),
    [
        pytest.param("1 + 1", "2", id="number"),
        pytest.param("[1, 'two', null]", "1,two,", id="array"),
        pytest.param("'b\\u00fccher \\u{1F600}'", "bücher \U0001f600", id="utf-8"),
        pytest.param("let a = 6; a * 7", "42", id="last-statement"),
        pytest.param("undefined", "undefined", id="undefined"),
    ],
)
def test_it_prints_what_the_script_returns_as_string_writes_it(program: Path, script: str, printed: str) -> None:
    assert _evaluate(program, script) == printed


def test_its_compilers_give_a_page_webassembly(program: Path) -> None:
    # V8 leaves WebAssembly out of the global object without its compilers.
    assert _evaluate(program, "typeof WebAssembly") == "object"


def test_v8_can_deoptimize_what_it_optimized(program: Path) -> None:
    # A loop that V8 optimizes and then deoptimizes. Its deoptimizer rewrites return addresses, for which Windows'
    # hardware-enforced shadow stacks end a program linked as compatible with them (native/page/BUILD.gn).
    assert _evaluate(program, "for (let i = 0; i < 200000; i++) new Array(64).fill(i); 'done'") == "done"


def test_intl_has_icus_whole_data(program: Path) -> None:
    # With only the few items that the library holds, Intl would know the root locale alone.
    script = """
        [
          Intl.DateTimeFormat.supportedLocalesOf(["ar", "de", "hi", "ja", "zh-Hant"]).length,
          Intl.supportedValuesOf("timeZone").length > 400,
          new Intl.Collator("de").resolvedOptions().locale,
        ].join(" ")
    """

    assert _evaluate(program, script) == "5 true de"


def test_a_script_that_throws_prints_what_it_threw(program: Path) -> None:
    result = _run(program, "throw new TypeError('no ' + 'good')")

    assert (result.returncode, result.stdout, result.stderr) == (_SCRIPT_THREW, "", "Uncaught TypeError: no good\n")


def test_a_script_that_does_not_compile_prints_why(program: Path) -> None:
    result = _run(program, "let = ;")

    assert result.returncode == _SCRIPT_THREW
    assert result.stderr.startswith("Uncaught SyntaxError: ")


def test_a_result_that_string_cannot_write_counts_as_thrown(program: Path) -> None:
    result = _run(program, "Symbol('x')")

    assert result.returncode == _SCRIPT_THREW
    assert result.stderr.startswith("Uncaught TypeError: ")


@pytest.mark.parametrize("arguments", [(), ("1", "2")], ids=["none", "two"])
def test_it_takes_exactly_one_argument(program: Path, arguments: tuple[str, ...]) -> None:
    result = _run(program, *arguments)

    assert result.returncode == _WRONG_ARGUMENTS
    assert result.stderr.startswith("usage: ")


# The messages of native/page_messages.h.
_HELLO = 1
_REFUSED = 2
_START = 3
_EVALUATE = 4
_EVALUATED = 5
_PROTOCOL_VERSION = 1
_MAX_PAYLOAD_SIZE = 16 * 1024 * 1024
_PAGE_REFUSED = 1
_DEFAULT_START = {"time_zone": "UTC", "locale": "en-US"}


class _Session:
    """Stands in for the library at the other end of a Page program's channel."""

    def __init__(self, program: Path) -> None:
        ours, theirs = socket.socketpair()
        self.socket = ours
        self.socket.settimeout(_TIMEOUT_SECONDS)
        self.process = subprocess.Popen(
            [program, f"--session={theirs.fileno()},{os.getpid()}"],
            pass_fds=[theirs.fileno()],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        theirs.close()

    def send(self, message_type: int, payload: bytes = b"") -> None:
        self.socket.sendall(struct.pack("<II", len(payload), message_type) + payload)

    def receive(self) -> tuple[int, bytes]:
        size, message_type = struct.unpack("<II", self._read(8))
        return message_type, self._read(size)

    def start(self, start: dict[str, str] = _DEFAULT_START) -> None:
        assert self.receive() == (_HELLO, struct.pack("<I", _PROTOCOL_VERSION))
        self.send(_HELLO, struct.pack("<I", _PROTOCOL_VERSION))
        self.send(_START, json.dumps(start).encode())

    def evaluate(self, script: str, call: int = 1) -> tuple[bool, str]:
        """Return whether the script failed, and its result as JSON or why it failed."""
        self.send(_EVALUATE, struct.pack("<Q", call) + script.encode())
        message_type, payload = self.receive()
        assert message_type == _EVALUATED, payload
        answered, outcome = struct.unpack("<QB", payload[:9])
        assert answered == call
        return outcome == 1, payload[9:].decode()

    def refusal(self) -> str:
        """Return why the Page refused what it got, and check that it ended for it."""
        message_type, payload = self.receive()
        assert message_type == _REFUSED, payload
        assert self.process.wait(_TIMEOUT_SECONDS) == _PAGE_REFUSED
        return payload.decode()

    def close(self) -> int:
        """Close the channel, and return the exit code of the Page program."""
        self.socket.close()
        returncode = self.process.wait(_TIMEOUT_SECONDS)
        assert self.process.stderr is not None
        assert b"Sanitizer" not in self.process.stderr.read()
        return returncode

    def _read(self, size: int) -> bytes:
        data = b""
        while len(data) < size:
            chunk = self.socket.recv(size - len(data))
            assert chunk, "the Page closed the channel"
            data += chunk
        return data


@pytest.fixture
def session(program: Path) -> Iterator[_Session]:
    """Start the Page program with a channel whose other end the test holds, on macOS and Linux."""
    if sys.platform == "win32":
        pytest.skip("the channel is a named pipe on Windows, which tests/test_page_process.py drives")
    page = _Session(program)
    yield page
    page.socket.close()
    if page.process.poll() is None:
        page.process.kill()
    page.process.wait(_TIMEOUT_SECONDS)


def test_a_page_says_hello_and_runs_scripts_until_the_channel_closes(session: _Session) -> None:
    session.start()

    assert session.evaluate("1 + 1") == (False, "2")
    assert session.close() == 0


@pytest.mark.parametrize(
    ("script", "json_text"),
    [
        pytest.param("'b\\u00fccher'", '"bücher"', id="string"),
        pytest.param("({a: [1, null, 'x']})", '{"a":[1,null,"x"]}', id="object"),
        pytest.param("undefined", "", id="undefined"),
        pytest.param("(function () {})", "", id="function"),
        pytest.param("'\\ud800'", '"\\ud800"', id="lone-surrogate"),
        pytest.param("({toJSON() { return 7; }})", "7", id="to-json"),
    ],
)
def test_a_result_comes_as_json_stringify_writes_it(session: _Session, script: str, json_text: str) -> None:
    session.start()

    assert session.evaluate(script) == (False, json_text)


@pytest.mark.parametrize(
    ("script", "why"),
    [
        pytest.param("throw new TypeError('no ' + 'good')", "Uncaught TypeError: no good", id="throws"),
        pytest.param("let = ;", "Uncaught SyntaxError: ", id="does-not-compile"),
        pytest.param("10n", "Uncaught TypeError: Do not know how to serialize a BigInt", id="bigint"),
        pytest.param(
            "const a = []; a.push(a); a", "Uncaught TypeError: Converting circular structure to JSON", id="cycle"
        ),
    ],
)
def test_a_script_that_fails_says_why_and_the_page_goes_on(session: _Session, script: str, why: str) -> None:
    session.start()

    failed, text = session.evaluate(script)

    assert failed
    assert text.startswith(why)
    assert session.evaluate("2 + 2", call=2) == (False, "4")


def test_scripts_share_one_context(session: _Session) -> None:
    session.start()

    assert session.evaluate("var total = 6", call=1) == (False, "")
    assert session.evaluate("total * 7", call=2) == (False, "42")


def test_a_page_reports_the_time_zone_and_locale_it_starts_with(session: _Session) -> None:
    session.start({"time_zone": "Asia/Tokyo", "locale": "de-DE"})
    script = """
        [
          Intl.DateTimeFormat().resolvedOptions().timeZone,
          new Date(0).getTimezoneOffset(),
          Intl.NumberFormat().resolvedOptions().locale,
          (1234.5).toLocaleString(),
        ].join(" ")
    """

    assert session.evaluate(script) == (False, '"Asia/Tokyo -540 de-DE 1.234,5"')


def test_a_page_refuses_a_time_zone_that_icu_does_not_know(session: _Session) -> None:
    session.start({"time_zone": "Mars/Olympus_Mons", "locale": "en-US"})

    assert session.refusal() == 'the time zone "Mars/Olympus_Mons" is unknown'


def test_a_page_refuses_another_version_of_the_messages(session: _Session) -> None:
    assert session.receive()[0] == _HELLO
    session.send(_HELLO, struct.pack("<I", 99))

    assert session.refusal() == "the library speaks version 99 of the messages, and the Page speaks 1"


@pytest.mark.parametrize(
    ("message_type", "payload", "why"),
    [
        pytest.param(77, b"", "message type 77 is unknown", id="unknown"),
        pytest.param(_EVALUATED, struct.pack("<QB", 1, 0), "message type 5 only goes to a Session", id="wrong-way"),
        pytest.param(_HELLO, struct.pack("<I", _PROTOCOL_VERSION), "a second hello", id="second-hello"),
        pytest.param(_START, b"[]", "a start that is not a JSON object", id="start-not-object"),
        pytest.param(_EVALUATE, b"\x01", "a script of 1 bytes", id="short-script"),
    ],
)
def test_a_page_refuses_a_message_it_does_not_know_and_says_why(
    session: _Session, message_type: int, payload: bytes, why: str
) -> None:
    session.start()

    session.send(message_type, payload)

    assert session.refusal() == why


def test_a_page_refuses_a_script_before_its_start(session: _Session) -> None:
    assert session.receive()[0] == _HELLO
    session.send(_HELLO, struct.pack("<I", _PROTOCOL_VERSION))
    session.send(_EVALUATE, struct.pack("<Q", 1) + b"1")

    assert session.refusal() == "a script came before the start"


def test_a_page_refuses_a_frame_larger_than_the_channel_carries(session: _Session) -> None:
    session.start()

    session.socket.sendall(struct.pack("<II", _MAX_PAYLOAD_SIZE + 1, _EVALUATE))

    assert session.refusal() == f"a frame of {_MAX_PAYLOAD_SIZE + 1} bytes is larger than the channel carries"


def test_a_page_ends_when_its_channel_closes_even_in_a_script_that_never_ends(session: _Session) -> None:
    session.start()
    session.send(_EVALUATE, struct.pack("<Q", 1) + b"while (true) {}")

    assert session.close() == 0


@pytest.mark.parametrize(
    "argument",
    ["--session=", "--session=3", "--session=x,1", "--session=1,1", "--session=0,1"],
    ids=["empty", "no-parent", "not-a-number", "standard-output", "standard-input"],
)
def test_a_session_argument_that_names_no_channel_is_wrong(program: Path, argument: str) -> None:
    result = _run(program, argument)

    assert result.returncode == _WRONG_ARGUMENTS
    assert result.stderr == "--session= names no channel and process\n"
