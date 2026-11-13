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
from typing import Any

import pytest

from mitto._persona import ENGINE_RELEASE_PERSONA, load_persona
from tools.parity import BASELINES_DIRECTORY

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
_CONSOLE = 6
_UNCAUGHT_ERROR = 7
_KNOWN_GAP = 18
_PROTOCOL_VERSION = 12
_MAX_PAYLOAD_SIZE = 16 * 1024 * 1024
_PAGE_REFUSED = 1
# What a Machine drawn from the table and an en-US Persona start a Page with.
_DEFAULT_START: dict[str, object] = {
    "time_zone": "America/New_York",
    "locale": "en-US",
    "processors": 8,
    "touch_points": 0,
    "work_area_x": 0,
    "work_area_y": 0,
    "work_area_width": 1920,
    "work_area_height": 1032,
    "window_state": "maximised",
    "window_x": 0,
    "window_y": 0,
    "window_width": 1920,
    "window_height": 1032,
    "contrast_theme": "none",
    "inverted_colors": "none",
    "memory": 8,
    "screen_width": 1920,
    "screen_height": 1080,
    "scale": 1,
    "viewport_width": 1920,
    "viewport_height": 945,
    "color_scheme": "light",
    "reduced_motion": "no-preference",
    "reduced_transparency": "no-preference",
    "effective_type": "4g",
    "navigator": {
        "user_agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko)"
        " Chrome/157.0.0.0 Safari/537.36",
        "platform": "Win32",
        "languages": "en-US,en",
        "user_agent_data": {
            "brands": [
                {"brand": "Not-A.Brand", "version": "99"},
                {"brand": "Google Chrome", "version": "157"},
                {"brand": "Chromium", "version": "157"},
            ],
            "full_version_list": [
                {"brand": "Not-A.Brand", "version": "99.0.0.0"},
                {"brand": "Google Chrome", "version": "157.0.8103.39"},
                {"brand": "Chromium", "version": "157.0.8103.39"},
            ],
            "mobile": False,
            "platform": "Windows",
            "platform_version": "19.0.0",
            "architecture": "x86",
            "bitness": "64",
            "model": "",
            "ua_full_version": "157.0.8103.39",
            "wow64": False,
            "form_factors": ["Desktop"],
        },
    },
    # Which the library adds: what about:blank's navigator.connection gives.
    "connection": {"rtt": 100, "downlink": 2.65},
    "features": {
        "enable_blink_features": [],
        "disable_blink_features": [],
        "v8_flags": [],
        "deprecated_unload": {"enabled": False, "allow_list": None, "by_bucket": None},
    },
}


def _with_features(**values: object) -> dict[str, object]:
    """Return the default start with each of `values` in place in its features."""
    features = _DEFAULT_START["features"]
    assert isinstance(features, dict)
    return {**_DEFAULT_START, "features": {**features, **values}}


class _Session:
    """Stands in for the library at the other end of a Page program's channel."""

    def __init__(self, program: Path) -> None:
        # The console messages and uncaught errors that came before a result, as (type, payload).
        self.reports: list[tuple[int, bytes]] = []
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

    def start(self, start: dict[str, object] = _DEFAULT_START) -> None:
        assert self.receive() == (_HELLO, struct.pack("<I", _PROTOCOL_VERSION))
        self.send(_HELLO, struct.pack("<I", _PROTOCOL_VERSION))
        self.send(_START, json.dumps(start).encode())

    def evaluate(self, script: str, call: int = 1) -> tuple[bool, str]:
        """Return whether the script failed, and its result as JSON or why it failed."""
        self.send(_EVALUATE, struct.pack("<Q", call) + script.encode())
        answered, failed, text = self.evaluated()
        assert answered == call
        return failed, text

    def evaluated(self) -> tuple[int, bool, str]:
        """Return the next result's call, whether it failed, and its result as JSON or why it failed.

        The console messages, uncaught errors and Known Gaps before it go to `reports`.
        """
        while (message := self.receive())[0] in {_CONSOLE, _UNCAUGHT_ERROR, _KNOWN_GAP}:
            self.reports.append(message)
        message_type, payload = message
        assert message_type == _EVALUATED, payload
        answered, outcome = struct.unpack("<QB", payload[:9])
        return answered, outcome == 1, payload[9:].decode()

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


@pytest.mark.parametrize(
    ("script", "outcome"),
    [
        pytest.param("Promise.resolve({a: 1})", (False, '{"a":1}'), id="fulfilled"),
        pytest.param("(async () => { await null; await null; return 'x'; })()", (False, '"x"'), id="async-function"),
        pytest.param(
            "Promise.reject(new RangeError('no'))", (True, "Uncaught (in promise) RangeError: no"), id="rejected"
        ),
        pytest.param(
            "Promise.resolve(10n)", (True, "Uncaught TypeError: Do not know how to serialize a BigInt"), id="bigint"
        ),
        pytest.param("({then(resolve) { resolve(1); }})", (False, "{}"), id="thenable-is-no-promise"),
    ],
)
def test_a_promise_gives_what_it_settles_with(session: _Session, script: str, outcome: tuple[bool, str]) -> None:
    session.start()

    assert session.evaluate(script) == outcome
    assert session.reports == []


def test_a_promise_that_a_later_script_settles_gives_its_result_then(session: _Session) -> None:
    session.start()
    session.send(_EVALUATE, struct.pack("<Q", 1) + b"new Promise(resolve => globalThis.settle = resolve)")
    session.send(_EVALUATE, struct.pack("<Q", 2) + b"settle([1, 2]); 'settled'")
    session.send(_EVALUATE, struct.pack("<Q", 3) + b"new Promise((_, reject) => globalThis.fail = reject)")
    session.send(_EVALUATE, struct.pack("<Q", 4) + b"fail(new Error('later')); 4")

    assert [session.evaluated() for _ in range(4)] == [
        (1, False, "[1,2]"),
        (2, False, '"settled"'),
        (3, True, "Uncaught (in promise) Error: later"),
        (4, False, "4"),
    ]
    assert session.reports == []


def test_a_promise_settled_by_a_later_script_gives_its_result_after_that_scripts_uncaught_errors(
    session: _Session,
) -> None:
    session.start()
    session.send(_EVALUATE, struct.pack("<Q", 1) + b"new Promise(resolve => globalThis.settle = resolve)")
    session.send(_EVALUATE, struct.pack("<Q", 2) + b"settle({toJSON() { Promise.reject('lost'); return 1; }}); 2")

    assert session.evaluated() == (1, False, "1")
    assert session.reports == [(_UNCAUGHT_ERROR, b"Uncaught (in promise) lost")]
    assert session.evaluated() == (2, False, "2")


def test_a_page_sends_what_its_scripts_write_to_the_console_before_their_result(session: _Session) -> None:
    session.start()
    script = """
        console.debug('d'); console.log('a', 1, [2, 3], {}); console.info('i'); console.warn('w');
        console.error('e', '\u00e9'); console.assert(false, 'f'); console.trace('t'); 0
    """

    assert session.evaluate(script) == (False, "0")
    assert session.reports == [
        (_CONSOLE, b"\x01d"),
        (_CONSOLE, b"\x02a 1 2,3 [object Object]"),
        (_CONSOLE, b"\x03i"),
        (_CONSOLE, b"\x04w"),
        (_CONSOLE, b"\x05e \xc3\xa9"),
        (_CONSOLE, b"\x05f"),
        (_CONSOLE, b"\x03t"),
    ]


def test_the_console_of_a_page_is_v8s_own_with_what_its_inspector_adds(session: _Session) -> None:
    session.start()

    assert session.evaluate("[typeof console.log, String(console.log), typeof console.createTask]") == (
        False,
        '["function","function log() { [native code] }","function"]',
    )


def test_a_page_reports_a_promise_rejected_with_no_handler_once_its_script_has_run(session: _Session) -> None:
    session.start()
    script = """
        Promise.reject(new Error('lost'));
        const handled = Promise.reject(new Error('handled'));
        handled.catch(() => {});
        Promise.resolve().then(() => { throw new TypeError('in a microtask'); });
        Promise.reject(Symbol('no string'));
        'done'
    """

    assert session.evaluate(script) == (False, '"done"')
    assert session.reports == [
        (_UNCAUGHT_ERROR, b"Uncaught (in promise) Error: lost"),
        (_UNCAUGHT_ERROR, b"Uncaught (in promise) exception"),
        (_UNCAUGHT_ERROR, b"Uncaught (in promise) TypeError: in a microtask"),
    ]


def test_scripts_share_one_context(session: _Session) -> None:
    session.start()

    assert session.evaluate("var total = 6", call=1) == (False, "")
    assert session.evaluate("total * 7", call=2) == (False, "42")


def test_a_page_reports_the_time_zone_and_locale_it_starts_with(session: _Session) -> None:
    session.start({**_DEFAULT_START, "time_zone": "Asia/Tokyo", "locale": "de-DE"})
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
    session.start({**_DEFAULT_START, "time_zone": "Mars/Olympus_Mons"})

    assert session.refusal() == 'the time zone "Mars/Olympus_Mons" is unknown'


def test_a_page_starts_with_the_features_and_v8_flags_of_its_start(session: _Session) -> None:
    session.start(
        _with_features(
            enable_blink_features=["HasUAVisualTransition", "CSSTextDecorationSkipSpaces"],
            disable_blink_features=["NewHTMLSettingMethods", "HasUAVisualTransition"],
            v8_flags=["--no-js_iterator_join", "--sqrt_allocation_limits"],
        )
    )
    script = "['afterHTML' in CharacterData.prototype, 'hasUAVisualTransition' in PopStateEvent.prototype,"
    script += " 'join' in Iterator.prototype]"

    assert session.evaluate(script) == (False, "[false,false,false]")


def test_a_page_with_no_features_in_its_start_has_the_engine_releases(session: _Session) -> None:
    session.start()
    script = "['afterHTML' in CharacterData.prototype, 'hasUAVisualTransition' in PopStateEvent.prototype,"
    script += " 'join' in Iterator.prototype]"

    assert session.evaluate(script) == (False, "[true,true,true]")


@pytest.mark.parametrize(
    ("start", "why"),
    [
        ({**_DEFAULT_START, "display_width": 1920}, 'a start with the unknown key "display_width"'),
        (
            {name: value for name, value in _DEFAULT_START.items() if name != "locale"},
            'a start whose "locale" is not a name',
        ),
        ({**_DEFAULT_START, "processors": 0}, 'a start whose "processors" is not a whole number from 1 to 64'),
        ({**_DEFAULT_START, "touch_points": 1.5}, 'a start whose "touch_points" is not a whole number from 0 to 256'),
        ({**_DEFAULT_START, "window_state": "first"}, 'a start whose "window_state" is not "maximised" or "normal"'),
        ({**_DEFAULT_START, "contrast_theme": "black"}, 'a start whose "contrast_theme" is not a contrast theme'),
        ({**_DEFAULT_START, "inverted_colors": True}, 'a start whose "inverted_colors" is not "none" or "inverted"'),
        ({**_DEFAULT_START, "memory": 12}, 'a start whose "memory" is not 2, 4, 8, 16 or 32'),
        ({**_DEFAULT_START, "scale": 0.5}, 'a start whose "scale" is not a number from 1 to 5'),
        (
            {**_DEFAULT_START, "viewport_height": 0},
            'a start whose "viewport_height" is not a whole number from 1 to 2147483647',
        ),
        ({**_DEFAULT_START, "color_scheme": "blue"}, 'a start whose "color_scheme" is not one of "light", "dark"'),
        (
            {**_DEFAULT_START, "effective_type": "5g"},
            'a start whose "effective_type" is not one of "slow-2g", "2g", "3g", "4g"',
        ),
        (
            {**_DEFAULT_START, "navigator": {"user_agent": "Mozilla/5.0"}},
            'a start whose "navigator" is not a user agent, a platform, languages and their data',
        ),
        (
            {name: value for name, value in _DEFAULT_START.items() if name != "connection"},
            'a start whose "connection" is not an rtt and a downlink that Chrome gives',
        ),
        (
            {**_DEFAULT_START, "connection": {"rtt": 125, "downlink": 2.65}},
            'a start whose "connection" is not an rtt and a downlink that Chrome gives',
        ),
        (
            {name: value for name, value in _DEFAULT_START.items() if name != "features"},
            'a start whose "features" is not an object',
        ),
        (_with_features(js_flags=[]), 'a start whose features have the unknown key "js_flags"'),
        (
            _with_features(enable_blink_features=["A,B"]),
            "a start whose features' \"enable_blink_features\" holds what is not a feature's name",
        ),
        (_with_features(v8_flags=["--a b"]), 'a start whose features\' "v8_flags" holds what is not a V8 flag'),
        (_with_features(disable_blink_features=["NoSuchFeature"]), "no runtime-enabled feature is named NoSuchFeature"),
        (
            _with_features(deprecated_unload=None),
            'a start whose features\' "deprecated_unload" is not an object',
        ),
        (
            _with_features(deprecated_unload={"enabled": True, "allow_list": None}),
            'a start whose features\' "deprecated_unload" has no "by_bucket"',
        ),
        (
            _with_features(deprecated_unload={"enabled": True, "allow_list": [""], "by_bucket": None}),
            'a start whose features\' "deprecated_unload" has an "allow_list" that is not a list of hosts',
        ),
        (
            _with_features(
                deprecated_unload={"enabled": True, "allow_list": None, "by_bucket": {"percent": 101, "bucket": 0}}
            ),
            'a start whose features\' "deprecated_unload" has a "by_bucket" that is not a percent and a bucket or null',
        ),
        (
            _with_features(deprecated_unload={"enabled": True, "allow_list": None, "by_bucket": None, "x": 1}),
            'a start whose features\' "deprecated_unload" has the unknown key "x"',
        ),
    ],
    ids=str,
)
def test_a_page_refuses_a_start_that_is_not_what_a_machine_gives(
    session: _Session, start: dict[str, object], why: str
) -> None:
    session.start(start)

    assert session.refusal() == why


def test_a_page_refuses_another_version_of_the_messages(session: _Session) -> None:
    assert session.receive()[0] == _HELLO
    session.send(_HELLO, struct.pack("<I", 99))

    assert (
        session.refusal() == f"the library speaks version 99 of the messages, and the Page speaks {_PROTOCOL_VERSION}"
    )


@pytest.mark.parametrize(
    ("message_type", "payload", "why"),
    [
        pytest.param(77, b"", "message type 77 is unknown", id="unknown"),
        pytest.param(_EVALUATED, struct.pack("<QB", 1, 0), "message type 5 only goes to a Session", id="wrong-way"),
        pytest.param(_CONSOLE, b"\x02log", "message type 6 only goes to a Session", id="console-wrong-way"),
        pytest.param(_UNCAUGHT_ERROR, b"no", "message type 7 only goes to a Session", id="uncaught-wrong-way"),
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


@pytest.mark.parametrize(
    "option", ["--enable-blink-features=HasUAVisualTransition", "--disable-blink-features=", "--html=<p>"]
)
@pytest.mark.skipif(sys.platform == "win32", reason="the channel is a named pipe on Windows")
def test_a_sessions_page_takes_its_features_and_document_from_the_session(program: Path, option: str) -> None:
    ours, theirs = socket.socketpair()
    with ours, theirs:
        result = subprocess.run(
            [program, option, f"--session={theirs.fileno()},{os.getpid()}"],
            pass_fds=[theirs.fileno()],
            capture_output=True,
            encoding="utf-8",
            timeout=_TIMEOUT_SECONDS,
            check=False,
        )

    assert result.returncode == _WRONG_ARGUMENTS
    assert result.stderr.startswith("usage: ")


# The messages of a Page's requests, in native/page_messages.h.
_REQUEST = 8
_CREDIT = 9
_GET_COOKIES = 10
_SET_COOKIE = 11
_LOADED = 12
_RESPONSE_STARTED = 13
_RESPONSE_BODY = 14
_RESPONSE_ENDED = 15
_COOKIES = 16
_COOKIE_STORE_GET = 19
_COOKIE_STORE_COOKIES = 22
_COOKIE_STORE_STORED = 23
_COOKIE_STORE_CHANGE = 24
_BODY_WINDOW = 64 * 1024
_SCRIPT_KIND = 1
_DOCUMENT_URL = "http://www.example.test/page"


def _sized(data: bytes) -> bytes:
    return struct.pack("<I", len(data)) + data


def _request(
    number: int,
    kind: int,
    url: str,
    method: str = "GET",
    headers: str = "",
    body: bytes | None = None,
    *,
    same_origin: bool = False,
) -> bytes:
    """The payload of a Request: its number, Kind, mode, URL, method and lines, and its body, if it has one."""
    has_body = b"\x00" if body is None else b"\x01" + body
    return (
        struct.pack("<QBB", number, kind, int(same_origin))
        + _sized(url.encode())
        + _sized(method.encode())
        + _sized(headers.encode())
        + has_body
    )


def _timing(
    times: tuple[int, ...] = (0,) * 10,
    flags: tuple[int, ...] = (0,) * 6,
    received_bytes: int = 0,
    redirect_count: int = 0,
    ends: tuple[int, int, int] = (0, 0, 0),
) -> bytes:
    """The Performance Timeline's part of a response: //net's ten times of its last hop, its six flags (socket reused,
    TAO passed, validated, cross-origin redirect, Critical-CH restart, opaque), the bytes that had come, its redirects,
    and when its first hop started, its last redirect ended and the navigation restarted for Critical-CH."""
    return struct.pack("<10q6BqI3q", *times, *flags, received_bytes, redirect_count, *ends)


def _respond(
    session: _Session,
    number: int,
    url: str,
    content_type: str,
    status: int = 200,
    *,
    status_text: str = "OK",
    redirected: bool = False,
    cors: bool = False,
    headers: str = "",
    connection: tuple[int, float] = (0, 0.0),
    timing: bytes | None = None,
) -> None:
    """Send the start of a response: for the navigation, `connection` is the rtt and downlink of its document."""
    lines = f"Content-Type: {content_type}\n{headers}".encode()
    session.send(
        _RESPONSE_STARTED,
        struct.pack("<QiBBBBqId", number, status, int(redirected), int(cors), 0, 0, 0, *connection)
        + (_timing() if timing is None else timing)
        + _sized(url.encode())
        + _sized(status_text.encode("latin-1"))
        + _sized(b"http/1.1")
        + _sized(b"unknown")
        + lines,
    )


def _body(session: _Session, number: int, data: bytes) -> None:
    session.send(_RESPONSE_BODY, struct.pack("<Q", number) + data)


def _ended(number: int, net_error: int = 0, totals: tuple[int, int, int] = (0, 0, 0)) -> bytes:
    """The payload of a Response ended: its net error, then its encoded body size, the bytes it received in all and
    when it completed."""
    return struct.pack("<Qi3q", number, net_error, *totals)


def _end(session: _Session, number: int, net_error: int = 0) -> None:
    session.send(_RESPONSE_ENDED, _ended(number, net_error))


def _nothing_comes(session: _Session, seconds: float = 0.3) -> bool:
    """Return whether the Page sends nothing for `seconds`."""
    session.socket.settimeout(seconds)
    try:
        session.socket.recv(1, socket.MSG_PEEK)
    except TimeoutError:
        return True
    finally:
        session.socket.settimeout(_TIMEOUT_SECONDS)
    return False


def test_a_page_loads_the_document_that_its_session_sends_and_asks_for_its_scripts(session: _Session) -> None:
    session.start()
    document = b"<!doctype html><p id=a>a</p><script src=app.js></script><p id=b>b</p>"
    _respond(session, 0, _DOCUMENT_URL, "text/html")
    _body(session, 0, document)

    # It takes what came, and asks for the script it reaches, with the URL resolved against the document's.
    assert session.receive() == (_CREDIT, struct.pack("<QI", 0, len(document)))
    assert session.receive() == (_REQUEST, _request(1, _SCRIPT_KIND, "http://www.example.test/app.js"))
    _end(session, 0)
    script = b"document.cookie = 'set=by script'; window.seen = [document.cookie, !!document.getElementById('b')];"
    _respond(session, 1, "http://www.example.test/app.js", "text/javascript")
    _body(session, 1, script)
    assert session.receive() == (_CREDIT, struct.pack("<QI", 1, len(script)))
    _end(session, 1)

    # The script's write goes first, then its read waits for the answer.
    assert session.receive() == (_SET_COOKIE, b"set=by script")
    message_type, question = session.receive()
    assert message_type == _GET_COOKIES
    session.send(_COOKIES, question + b"from=session; set=by script")
    assert session.receive() == (_LOADED, b"")

    assert session.evaluate("[document.URL, document.readyState, seen, document.body.lastChild.id]") == (
        False,
        f'["{_DOCUMENT_URL}","complete",["from=session; set=by script",false],"b"]',
    )
    assert session.close() == 0


def test_a_documents_event_handler_attributes_run_as_it_loads(session: _Session) -> None:
    # The handlers are listeners of their targets, which the loading's events reach in Chrome's order.
    session.start()
    document = (
        b"<!doctype html><script>window.seen = [];"
        b"document.onreadystatechange = () => seen.push(document.readyState);"
        b"window.onload = (event) => seen.push('load ' + event.target.nodeName);</script>"
    )
    _respond(session, 0, _DOCUMENT_URL, "text/html")
    _body(session, 0, document)
    assert session.receive() == (_CREDIT, struct.pack("<QI", 0, len(document)))
    _end(session, 0)
    assert session.receive() == (_LOADED, b"")

    assert session.evaluate("seen.join()") == (False, '"interactive,complete,load #document"')
    assert session.close() == 0


def test_a_page_gives_credit_only_for_what_its_parser_takes(session: _Session) -> None:
    session.start()
    first = b"<script src=/wait.js></script>"
    second = b"<p id=later>" + b"x" * 2000 + b"</p>"
    _respond(session, 0, _DOCUMENT_URL, "text/html")
    _body(session, 0, first)
    assert session.receive()[0] == _CREDIT
    assert session.receive()[0] == _REQUEST

    # The parser waits for the script, so what comes meanwhile waits too, and the Session gets no room for more.
    _body(session, 0, second)
    assert _nothing_comes(session)

    _respond(session, 1, "http://www.example.test/wait.js", "text/javascript")
    _end(session, 1)
    assert session.receive() == (_CREDIT, struct.pack("<QI", 0, len(second)))
    _end(session, 0)
    assert session.receive() == (_LOADED, b"")
    assert session.evaluate("document.getElementById('later').textContent.length") == (False, "2000")


_FETCH_KIND = 2
_CANCEL = 17


def _loaded(session: _Session) -> None:
    """Give the Page an empty document, and wait until it has loaded."""
    _respond(session, 0, _DOCUMENT_URL, "text/html")
    _body(session, 0, b"<p>fetch</p>")
    assert session.receive()[0] == _CREDIT
    _end(session, 0)
    assert session.receive() == (_LOADED, b"")


# What a page reads of the browser and the machine, from what the Page starts with.
_NAVIGATOR_AND_SCREEN = """
    ({
      userAgent: navigator.userAgent,
      appVersion: navigator.appVersion,
      platform: navigator.platform,
      language: navigator.language,
      languages: navigator.languages,
      hardwareConcurrency: navigator.hardwareConcurrency,
      deviceMemory: navigator.deviceMemory,
      maxTouchPoints: navigator.maxTouchPoints,
      brands: navigator.userAgentData.brands,
      mobile: navigator.userAgentData.mobile,
      uaPlatform: navigator.userAgentData.platform,
      screen: [screen.width, screen.height, screen.availLeft, screen.availTop, screen.availWidth, screen.availHeight],
      window: [screenX, screenY, screenLeft, screenTop, outerWidth, outerHeight, innerWidth, innerHeight],
      devicePixelRatio,
      connection: [navigator.connection.effectiveType, navigator.connection.rtt, navigator.connection.downlink],
    })
"""


def test_a_page_reads_the_browser_and_the_machine_that_it_starts_with(session: _Session) -> None:
    session.start()

    failed, text = session.evaluate(_NAVIGATOR_AND_SCREEN)

    assert not failed, text
    navigator: Any = _DEFAULT_START["navigator"]
    data = navigator["user_agent_data"]
    assert json.loads(text) == {
        "userAgent": navigator["user_agent"],
        "appVersion": navigator["user_agent"].removeprefix("Mozilla/"),
        "platform": "Win32",
        "language": "en-US",
        "languages": ["en-US", "en"],
        "hardwareConcurrency": 8,
        "deviceMemory": 8,
        "maxTouchPoints": 0,
        "brands": data["brands"],
        "mobile": False,
        "uaPlatform": "Windows",
        "screen": [1920, 1080, 0, 0, 1920, 1032],
        "window": [0, 0, 0, 0, 1920, 1032, 1920, 945],
        "devicePixelRatio": 1,
        "connection": ["4g", 100, 2.65],
    }


def test_a_page_tells_its_session_of_each_known_gap_once_for_each_document(session: _Session) -> None:
    # A constructor, then an attribute whose setter and getter are one Known Gap, as Chrome's use counters count one
    # feature once for each document.
    script = "const h = new Highlight(); h.priority = 2; [h.priority, h.priority, new Highlight().priority].join()"
    session.start()

    assert session.evaluate(script) == (False, '"2,2,0"')
    assert session.evaluate("new Highlight().priority") == (False, "0")
    _respond(session, 0, _DOCUMENT_URL, "text/html")
    _body(session, 0, b"<p>another document</p>")
    assert session.receive()[0] == _CREDIT
    _end(session, 0)
    assert session.receive() == (_LOADED, b"")
    assert session.evaluate("new Highlight().priority") == (False, "0")
    assert session.reports == [
        (_KNOWN_GAP, b"new Highlight"),
        (_KNOWN_GAP, b"Highlight.prototype.priority"),
        (_KNOWN_GAP, b"new Highlight"),
        (_KNOWN_GAP, b"Highlight.prototype.priority"),
    ]


def test_a_page_takes_its_documents_connection_from_the_navigation(session: _Session) -> None:
    session.start()
    _respond(session, 0, _DOCUMENT_URL, "text/html", connection=(150, 2.4))
    _body(session, 0, b"<p>connection</p>")
    assert session.receive()[0] == _CREDIT
    _end(session, 0)
    assert session.receive() == (_LOADED, b"")

    assert session.evaluate("[navigator.connection.rtt, navigator.connection.downlink]") == (False, "[150,2.4]")


@pytest.mark.parametrize(
    ("languages", "expected"),
    [
        pytest.param("de-DE,de,en", ["de-DE", "de", "en"], id="three"),
        pytest.param(" en_GB , fr ", ["en-GB", "fr"], id="sanitized"),
        pytest.param("", ["en-US"], id="none-gives-the-locale"),
    ],
)
def test_a_page_reads_its_languages_as_chrome_parses_its_setting(
    session: _Session, languages: str, expected: list[str]
) -> None:
    navigator: Any = _DEFAULT_START["navigator"]
    session.start({**_DEFAULT_START, "navigator": {**navigator, "languages": languages}})

    failed, text = session.evaluate("[navigator.language, navigator.languages]")

    assert (failed, json.loads(text)) == (False, [expected[0], expected])


def test_high_entropy_values_resolve_with_what_the_hints_send(session: _Session) -> None:
    session.start()
    script = """
        navigator.userAgentData.getHighEntropyValues(
          ["wow64", "platformVersion", "architecture", "model", "bitness", "uaFullVersion", "fullVersionList",
           "formFactors", "unknown"])
          .then((values) => [Object.keys(values), values])
    """

    failed, text = session.evaluate(script)

    assert not failed, text
    navigator: Any = _DEFAULT_START["navigator"]
    data = navigator["user_agent_data"]
    keys, values = json.loads(text)
    # A dictionary's members in their sorted order.
    assert keys == sorted(keys)
    assert values == {
        "architecture": "x86",
        "bitness": "64",
        "brands": data["brands"],
        "formFactors": ["Desktop"],
        "fullVersionList": data["full_version_list"],
        "mobile": False,
        "model": "",
        "platform": "Windows",
        "platformVersion": "19.0.0",
        "uaFullVersion": "157.0.8103.39",
        "wow64": False,
    }


def test_screen_orientation_cannot_be_locked_as_on_windows(session: _Session) -> None:
    session.start()
    script = """
        screen.orientation.lock("portrait").then(
          () => "locked",
          (error) => [error.name, error.message, screen.orientation.type, screen.orientation.angle])
    """

    assert session.evaluate(script) == (
        False,
        '["NotSupportedError","screen.orientation.lock() is not available on this device.","landscape-primary",0]',
    )


def test_a_fetch_asks_the_session_for_what_its_script_gives_and_resolves_with_the_response(session: _Session) -> None:
    session.start()
    _loaded(session)
    session.send(
        _EVALUATE,
        struct.pack("<Q", 1) + b"fetch('/data', {method: 'POST', body: 'hi', headers: {'X-A': '1'}})"
        b".then(async r => [r.status, r.statusText, r.redirected, r.headers.get('x-b'), await r.text()])",
    )

    assert session.receive() == (
        _REQUEST,
        _request(
            1,
            _FETCH_KIND,
            "http://www.example.test/data",
            "POST",
            "X-A: 1\nContent-Type: text/plain;charset=UTF-8\n",
            b"hi",
        ),
    )
    _respond(
        session,
        1,
        "http://www.example.test/moved",
        "text/plain",
        201,
        status_text="Made",
        redirected=True,
        headers="X-B: 2\n",
    )
    _body(session, 1, b"the body")
    assert session.receive() == (_CREDIT, struct.pack("<QI", 1, 8))
    _end(session, 1)
    assert session.evaluated() == (1, False, '[201,"Made",true,"2","the body"]')
    assert session.close() == 0


def test_a_fetchs_body_gets_credit_only_once_a_script_reads_it(session: _Session) -> None:
    session.start()
    _loaded(session)
    session.send(
        _EVALUATE, struct.pack("<Q", 1) + b"window.got = fetch('/large').then(r => { window.response = r; }); null"
    )
    # fetch() asks the Session at once, before the script that calls it returns.
    assert session.receive()[0] == _REQUEST
    assert session.evaluated() == (1, False, "null")
    _respond(session, 1, "http://www.example.test/large", "text/plain")
    _body(session, 1, b"x" * _BODY_WINDOW)

    # The Response holds its window, and no script reads it, so the Session gets no room for more.
    assert _nothing_comes(session)

    session.send(_EVALUATE, struct.pack("<Q", 2) + b"response.text().then(text => text.length)")
    assert session.receive() == (_CREDIT, struct.pack("<QI", 1, _BODY_WINDOW))
    _body(session, 1, b"y" * 10)
    assert session.receive() == (_CREDIT, struct.pack("<QI", 1, 10))
    _end(session, 1)
    assert session.evaluated() == (2, False, str(_BODY_WINDOW + 10))
    assert session.close() == 0


def test_a_fetch_that_the_session_fails_rejects_and_a_failed_body_rejects_its_read(session: _Session) -> None:
    session.start()
    _loaded(session)
    session.send(_EVALUATE, struct.pack("<Q", 1) + b"fetch('/a').then(() => 'ok', e => String(e))")
    assert session.receive()[0] == _REQUEST
    _end(session, 1, -102)
    assert session.evaluated() == (1, False, '"TypeError: Failed to fetch"')

    session.send(_EVALUATE, struct.pack("<Q", 2) + b"fetch('/b').then(r => r.text()).then(() => 'ok', e => String(e))")
    assert session.receive()[0] == _REQUEST
    _respond(session, 2, "http://www.example.test/b", "text/plain")
    _body(session, 2, b"part")
    assert session.receive()[0] == _CREDIT
    _end(session, 2, -100)
    assert session.evaluated() == (2, False, '"TypeError: Failed to fetch"')
    assert session.close() == 0


def test_a_fetch_whose_body_cannot_fit_in_a_frame_rejects_and_the_page_goes_on(session: _Session) -> None:
    session.start()
    _loaded(session)
    script = (
        b"fetch('/upload', {method: 'POST', body: new Uint8Array(16 * 1024 * 1024)})"
        b".then(() => 'sent', error => String(error))"
    )

    assert session.evaluate(script.decode()) == (False, '"TypeError: Failed to fetch"')
    assert session.evaluate("1 + 1", call=2) == (False, "2")
    assert session.close() == 0


def test_a_cors_response_shows_only_the_header_names_that_cors_lets_through(session: _Session) -> None:
    # The Session says the response's tainting is cors, which a redirect back to the document's origin keeps.
    session.start()
    _loaded(session)
    session.send(
        _EVALUATE,
        struct.pack("<Q", 1) + b"fetch('/back').then(r => [r.type, r.headers.get('x-hidden'), r.headers.get('x-shown'),"
        b" r.headers.get('content-type')])",
    )
    assert session.receive()[0] == _REQUEST
    _respond(
        session,
        1,
        _DOCUMENT_URL,
        "text/plain",
        redirected=True,
        cors=True,
        headers="X-Hidden: 1\nX-Shown: 2\nAccess-Control-Expose-Headers: X-Shown\n",
    )
    _end(session, 1)
    assert session.evaluated() == (1, False, '["cors",null,"2","text/plain"]')
    assert session.close() == 0


def test_timers_run_while_the_page_waits_for_a_response(session: _Session) -> None:
    session.start()
    _loaded(session)
    session.send(
        _EVALUATE,
        struct.pack("<Q", 1) + b"window.ticks = 0; const id = setInterval(() => ++ticks, 5);"
        b"fetch('/slow').then(r => r.text()).then(text => { clearInterval(id); return [text, ticks > 2]; })",
    )
    assert session.receive()[0] == _REQUEST
    assert _nothing_comes(session, 0.2)
    _respond(session, 1, "http://www.example.test/slow", "text/plain")
    _end(session, 1)
    assert session.evaluated() == (1, False, '["",true]')
    assert session.close() == 0


def test_a_page_refuses_more_of_a_body_than_it_has_given_credit_for(session: _Session) -> None:
    session.start()
    first = b"<script src=/wait.js></script>"
    _respond(session, 0, _DOCUMENT_URL, "text/html")
    _body(session, 0, first)
    assert session.receive()[0] == _CREDIT
    assert session.receive()[0] == _REQUEST

    _body(session, 0, b"x" * _BODY_WINDOW)
    _body(session, 0, b"x")

    assert session.refusal() == "more of the body of request 0 than the Page has given credit for"


@pytest.mark.parametrize(
    ("message_type", "payload", "why"),
    [
        pytest.param(
            _RESPONSE_BODY,
            struct.pack("<Q", 5) + b"x",
            "a body of request 5, which is not in progress",
            id="body-of-no-request",
        ),
        pytest.param(_RESPONSE_ENDED, _ended(0), "the end of request 0, which is not in progress", id="end"),
        pytest.param(
            _COOKIES, struct.pack("<Q", 1) + b"a=b", "cookies for question 1, which no script waits for", id="cookies"
        ),
        pytest.param(
            _COOKIE_STORE_COOKIES,
            struct.pack("<QI", 3, 0),
            "cookies for CookieStore question 3, which no script waits for",
            id="cookie-store-cookies",
        ),
        pytest.param(
            _COOKIE_STORE_STORED,
            struct.pack("<QB", 4, 1),
            "an answer for CookieStore question 4, which no script waits for",
            id="cookie-store-stored",
        ),
        pytest.param(
            _COOKIE_STORE_COOKIES,
            struct.pack("<QI", 3, 1),
            "cookies of a CookieStore that are not valid",
            id="cookie-store-missing-cookie",
        ),
        pytest.param(
            _COOKIE_STORE_STORED, struct.pack("<QB", 4, 2), "a CookieStore answer of 9 bytes", id="cookie-store-byte-2"
        ),
        pytest.param(
            _COOKIE_STORE_CHANGE,
            b"\x01\x08\x00" + bytes(8) + bytes(16),
            "a change of cookies that is not valid",
            id="cookie-store-unknown-flag",
        ),
        pytest.param(
            _COOKIE_STORE_GET, struct.pack("<QB", 1, 1), "message type 19 only goes to a Session", id="cookie-store-get"
        ),
        pytest.param(_REQUEST, struct.pack("<QB", 1, 1) + b"x", "message type 8 only goes to a Session", id="request"),
        pytest.param(_RESPONSE_ENDED, _ended(0, 1), "the end of a response with the net error 1", id="positive-error"),
        pytest.param(
            _RESPONSE_ENDED, struct.pack("<Qi", 0, 0), "an end of a response of 12 bytes", id="end-without-totals"
        ),
        pytest.param(
            _RESPONSE_ENDED,
            _ended(0, 0, (-1, 0, 0)),
            "the end of a response that //net cannot give",
            id="negative-body-size",
        ),
    ],
)
def test_a_page_refuses_what_a_session_should_not_send_of_its_requests(
    session: _Session, message_type: int, payload: bytes, why: str
) -> None:
    session.start()

    session.send(message_type, payload)

    assert session.refusal() == why


@pytest.mark.parametrize(
    "timing",
    [
        pytest.param(_timing(times=(-1,) + (0,) * 9), id="negative-time"),
        pytest.param(_timing(flags=(2, 0, 0, 0, 0, 0)), id="flag-of-two"),
        pytest.param(_timing(received_bytes=-1), id="negative-bytes"),
        pytest.param(_timing(redirect_count=21), id="more-redirects-than-net-follows"),
        pytest.param(_timing(ends=(0, -5, 0)), id="negative-redirect-end"),
    ],
)
def test_a_page_refuses_a_response_whose_timing_net_cannot_give(session: _Session, timing: bytes) -> None:
    session.start()

    _respond(session, 0, _DOCUMENT_URL, "text/html", timing=timing)

    assert session.refusal() == "a response whose timing //net cannot give"


def test_a_page_refuses_a_response_both_cors_and_opaque(session: _Session) -> None:
    session.start()

    _respond(session, 0, _DOCUMENT_URL, "text/html", cors=True, timing=_timing(flags=(0, 0, 0, 0, 0, 1)))

    assert session.refusal() == "a response whose timing //net cannot give"


def test_a_document_of_a_session_runs_in_a_context_of_its_own_and_ends_waiting_calls(session: _Session) -> None:
    session.start()
    session.send(_EVALUATE, struct.pack("<Q", 1) + b"window.before = 1; new Promise(() => {})")
    _respond(session, 0, _DOCUMENT_URL, "text/html")

    # The document's response replaces the context, as a navigation does.
    assert session.evaluated() == (1, True, "Execution context was destroyed.")
    _body(session, 0, b"<p>new</p>")
    assert session.receive()[0] == _CREDIT
    _end(session, 0)
    assert session.receive() == (_LOADED, b"")
    assert session.evaluate("[typeof before, document.body.innerHTML]") == (False, '["undefined","<p>new</p>"]')


def test_notification_permission_is_denied_where_the_document_is_not_a_secure_context(session: _Session) -> None:
    # The document's URL is not potentially trustworthy. Chrome 157 gave the same for a page at that URL, which a
    # resolver rule sent to a local server: "denied" without a prompt, and the deprecated callback before the promise.
    session.start()
    _respond(session, 0, _DOCUMENT_URL, "text/html")
    _body(session, 0, b"<p>insecure</p>")
    assert session.receive()[0] == _CREDIT
    _end(session, 0)
    assert session.receive() == (_LOADED, b"")
    script = """(async () => {
        const seen = [isSecureContext, Notification.permission];
        const answer = Notification.requestPermission((permission) => seen.push("callback " + permission));
        seen.push("returned");
        seen.push("resolved " + await answer);
        return seen;
    })()"""

    assert session.evaluate(script) == (False, '[false,"denied","returned","callback denied","resolved denied"]')


def test_permissions_are_denied_where_the_document_is_not_a_secure_context(session: _Session) -> None:
    # Chrome 157 gave the same at that URL: every permission context is restricted to secure origins, but the storage
    # access grant's, which turns "denied" into "prompt". Storage is not exposed, and geolocation fails at once.
    session.start()
    _respond(session, 0, _DOCUMENT_URL, "text/html")
    _body(session, 0, b"<p>insecure</p>")
    assert session.receive()[0] == _CREDIT
    _end(session, 0)
    assert session.receive() == (_LOADED, b"")
    names = [
        "accelerometer",
        "background-fetch",
        "background-sync",
        "camera",
        "captured-surface-control",
        "clipboard-read",
        "clipboard-write",
        "display-capture",
        "geolocation",
        "gyroscope",
        "idle-detection",
        "keyboard-lock",
        "local-fonts",
        "local-network",
        "local-network-access",
        "loopback-network",
        "magnetometer",
        "microphone",
        "midi",
        "notifications",
        "payment-handler",
        "periodic-background-sync",
        "persistent-storage",
        "pointer-lock",
        "screen-wake-lock",
        "storage-access",
        "window-management",
    ]
    script = f"""(async () => {{
        const states = {{}};
        for (const name of {json.dumps(names)}) {{
            const status = await navigator.permissions.query({{name}});
            states[name] = status.state + "/" + status.name;
        }}
        const geolocation = await new Promise((resolve) => navigator.geolocation.getCurrentPosition(
            () => resolve("position"), (error) => resolve(error.code + " " + error.message)));
        return [JSON.stringify(states), await document.hasStorageAccess(), typeof navigator.storage,
            navigator.getGamepads().length, geolocation];
    }})()"""

    assert session.evaluate(script) == (
        False,
        json.dumps(
            [
                '{"accelerometer":"denied/sensors","background-fetch":"denied/background_fetch","background-sync":"denied/background_sync","camera":"denied/video_capture","captured-surface-control":"denied/captured-surface-control","clipboard-read":"denied/clipboard_read","clipboard-write":"denied/clipboard_write","display-capture":"denied/display_capture","geolocation":"denied/geolocation","gyroscope":"denied/sensors","idle-detection":"denied/idle_detection","keyboard-lock":"denied/keyboard-lock","local-fonts":"denied/local_fonts","local-network":"denied/local-network","local-network-access":"denied/local-network-access","loopback-network":"denied/loopback-network","magnetometer":"denied/sensors","microphone":"denied/audio_capture","midi":"denied/midi","notifications":"denied/notifications","payment-handler":"denied/payment_handler","periodic-background-sync":"denied/periodic_background_sync","persistent-storage":"denied/durable_storage","pointer-lock":"denied/pointer-lock","screen-wake-lock":"denied/screen_wake_lock","storage-access":"prompt/storage-access","window-management":"denied/window-management"}',
                False,
                "undefined",
                8,
                "1 Only secure origins are allowed (see: https://goo.gl/Y0ZkNV).",
            ],
            separators=(",", ":"),
        ),
    )


@pytest.mark.parametrize("host", ["127.0.0.1", "www.example.test"])
def test_a_documents_feature_policy_allows_what_chromes_allows(session: _Session, host: str) -> None:
    # Chrome 157 gave the same at http://HOST:8722/, from a new profile, whose seed deprecates nothing yet: a feature
    # whose default allowlist is * is the "*" one's, one whose default is 'self' the document's origin's, an unknown
    # feature nobody's, and a URL that does not parse has an opaque origin, which has none.
    origin = f"http://{host}:8722"
    session.start()
    _respond(session, 0, f"{origin}/", "text/html")
    _body(session, 0, b"<p>policy</p>")
    assert session.receive()[0] == _CREDIT
    _end(session, 0)
    assert session.receive() == (_LOADED, b"")
    script = """(() => {
        const p = document.featurePolicy;
        return [[p.allowsFeature("camera"), p.allowsFeature("autoplay"), p.allowsFeature("unknown"),
            p.allowsFeature("camera", "https://other.example"), p.allowsFeature("autoplay", "https://other.example"),
            p.allowsFeature("camera", location.origin), p.allowsFeature("unload"), p.allowsFeature("camera", "not a url")],
            ["camera", "autoplay", "unknown", "unload", "fullscreen", "payment"].map((f) => p.getAllowlistForFeature(f))];
    })()"""

    assert session.evaluate(script) == (
        False,
        json.dumps(
            [
                [True, True, False, False, False, True, True, False],
                [[origin], [origin], [], ["*"], [origin], [origin]],
            ],
            separators=(",", ":"),
        ),
    )


def _load_policy_page(session: _Session, url: str) -> None:
    _respond(session, 0, url, "text/html")
    _body(session, 0, b"<p>policy</p>")
    assert session.receive()[0] == _CREDIT
    _end(session, 0)
    assert session.receive() == (_LOADED, b"")


_UNLOAD_SCRIPT = """(() => {
    const p = document.featurePolicy;
    return [p.features().includes("unload"), p.allowedFeatures().includes("unload"), p.allowsFeature("unload"),
        p.getAllowlistForFeature("unload")];
})()"""
_UNLOAD_KEPT = json.dumps([True, True, True, ["*"]], separators=(",", ":"))
_UNLOAD_DEPRECATED = json.dumps([True, False, False, []], separators=(",", ":"))


@pytest.mark.parametrize(
    ("unload", "deprecated"),
    [
        ({"enabled": False, "allow_list": None, "by_bucket": None}, False),
        # Neither an allow list nor buckets: every page.
        ({"enabled": True, "allow_list": None, "by_bucket": None}, True),
        ({"enabled": True, "allow_list": ["a.test", "www.example.test"], "by_bucket": None}, True),
        ({"enabled": True, "allow_list": ["a.test"], "by_bucket": None}, False),
        # An empty allow list covers every host.
        ({"enabled": True, "allow_list": [], "by_bucket": None}, True),
        # A host off the allow list is in the buckets' share.
        ({"enabled": True, "allow_list": ["a.test"], "by_bucket": {"percent": 100, "bucket": None}}, True),
        ({"enabled": True, "allow_list": None, "by_bucket": {"percent": 0, "bucket": 1}}, False),
        # IsIncludedInGradualRollout() with base::PersistentHash(): www.example.test is in half of the hosts of bucket 1,
        # and not of bucket 0.
        ({"enabled": True, "allow_list": None, "by_bucket": {"percent": 50, "bucket": 1}}, True),
        ({"enabled": True, "allow_list": None, "by_bucket": {"percent": 50, "bucket": 0}}, False),
        # Without a bucket the Persona is in no share.
        ({"enabled": True, "allow_list": None, "by_bucket": {"percent": 50, "bucket": None}}, False),
    ],
    ids=str,
)
def test_the_seed_takes_unload_out_of_the_policy_of_the_pages_it_covers(
    session: _Session, unload: dict[str, object], deprecated: bool
) -> None:
    # GetPermissionsPolicyFeatureList(): the default allowlist of unload is none for a covered page, so no origin has
    # it, but it is still one of the features.
    session.start(_with_features(deprecated_unload=unload))
    _load_policy_page(session, "http://www.example.test:8722/")

    assert session.evaluate(_UNLOAD_SCRIPT) == (False, _UNLOAD_DEPRECATED if deprecated else _UNLOAD_KEPT)


@pytest.mark.parametrize(
    ("unload", "deprecated"),
    [
        ({"enabled": True, "allow_list": None, "by_bucket": None}, True),
        # Either list limits it to pages of http and https.
        ({"enabled": True, "allow_list": [], "by_bucket": None}, False),
        ({"enabled": True, "allow_list": None, "by_bucket": {"percent": 100, "bucket": 0}}, False),
    ],
    ids=str,
)
def test_unload_stays_on_a_page_that_is_not_http_where_the_seed_limits_it(
    session: _Session, unload: dict[str, object], deprecated: bool
) -> None:
    session.start(_with_features(deprecated_unload=unload))

    assert session.evaluate(_UNLOAD_SCRIPT) == (False, _UNLOAD_DEPRECATED if deprecated else _UNLOAD_KEPT)


def test_a_page_of_the_engine_releases_persona_lists_the_features_of_its_baselines(session: _Session) -> None:
    # What the Persona's Page turns off, TopicsAPI among them, and its seed, which deprecates unload on every host.
    persona = load_persona(ENGINE_RELEASE_PERSONA)
    session.start({**_DEFAULT_START, "features": persona.page_features.to_option()})
    _load_policy_page(session, "http://www.example.test:8722/")
    page = BASELINES_DIRECTORY / persona.release / "script" / "page.json"
    values = json.loads(page.read_text(encoding="utf-8"))["values"]

    assert session.evaluate("[document.featurePolicy.features(), document.featurePolicy.allowedFeatures()]") == (
        False,
        json.dumps(
            [
                values["document.featurePolicy.features()"]["value"],
                values["document.featurePolicy.allowedFeatures()"]["value"],
            ],
            separators=(",", ":"),
        ),
    )


_FILE_SYSTEM_SCRIPT = """new Promise((resolve) => {
    const order = [];
    webkitRequestFileSystem(TEMPORARY, 1024, (fs) => {
        const root = fs.root;
        resolve([order.concat("callback").join(), fs.name, Object.prototype.toString.call(fs), root === fs.root,
            root.filesystem === fs, root.isDirectory, root.isFile, root.name, root.fullPath, root.toURL()]);
    });
    order.push("returned");
})"""


@pytest.mark.parametrize("host", ["127.0.0.1", "www.example.test"])
def test_webkit_request_file_system_gives_the_file_system_of_the_origin(session: _Session, host: str) -> None:
    # Chrome 157 gave the same name, root and URL at http://HOST:8722/, from a new profile, on a secure page and
    # elsewhere: Chrome's identifier of the origin, and its root URL of a temporary file system.
    session.start()
    _load_policy_page(session, f"http://{host}:8722/")

    assert session.evaluate(_FILE_SYSTEM_SCRIPT) == (
        False,
        json.dumps(
            [
                "returned,callback",
                f"http_{host}_8722:Temporary",
                "[object DOMFileSystem]",
                True,
                True,
                True,
                False,
                "",
                "/",
                f"filesystem:http://{host}:8722/temporary/",
            ],
            separators=(",", ":"),
        ),
    )


def test_webkit_request_file_system_names_a_persistent_one_and_refuses_another_type(session: _Session) -> None:
    # As Chrome 157 gave at http://127.0.0.1:8722/. A port that is the scheme's default is 0 in the name, as
    # storage::GetIdentifierFromOrigin() writes it.
    session.start()
    _load_policy_page(session, "https://www.example.test/")
    script = """Promise.all([
        new Promise((resolve) => webkitRequestFileSystem(PERSISTENT, 0, (fs) => resolve(fs.name))),
        new Promise((resolve) => webkitRequestFileSystem(7, 0, () => resolve("opened"),
            (error) => resolve(error.name + " " + error.message))),
        new Promise((resolve) => webkitRequestFileSystem(0, 0, {handleEvent(fs) { resolve(this.tag + fs.name); },
            tag: "object "})),
    ])"""

    assert session.evaluate(script) == (
        False,
        json.dumps(
            [
                "https_www.example.test_0:Persistent",
                "InvalidModificationError The object can not be modified in this way.",
                "object https_www.example.test_0:Temporary",
            ],
            separators=(",", ":"),
        ),
    )


def test_a_presentation_availability_outlives_its_request_until_its_promise_settles(session: _Session) -> None:
    # Blink's resolver holds what it resolves with, so a page that keeps only the promise still gets the availability.
    session.start(_with_features(v8_flags=["--expose-gc"]))
    script = """new Promise((resolve) => {
        const promise = new PresentationRequest("https://a.test/").getAvailability();
        gc();
        promise.then((availability) => resolve(String(availability) + " " + availability.value));
    })"""

    assert session.evaluate(script) == (False, json.dumps("[object PresentationAvailability] false"))


def test_webkit_request_file_system_refuses_an_opaque_origin(session: _Session) -> None:
    # SecurityOrigin::CanAccessFileSystem(): about:blank, which the Page starts with, has an opaque origin.
    session.start()
    script = """new Promise((resolve) => webkitRequestFileSystem(0, 0, () => resolve("opened"),
        (error) => resolve(error.name + ": " + error.message)))"""

    assert session.evaluate(script) == (
        False,
        json.dumps(
            "SecurityError: It was determined that certain files are unsafe for access within a Web application, or"
            " that too many calls are being made on file resources."
        ),
    )


def test_a_page_decodes_its_document_as_utf8_across_the_pieces_it_comes_in(session: _Session) -> None:
    session.start()
    document = "﻿<p id=text>café 𝄞</p>".encode()
    _respond(session, 0, _DOCUMENT_URL, "text/html")
    # The byte order mark, é and 𝄞 each cut between two pieces.
    pieces = [document[:1], document[1:2], document[2:18], document[18:21], document[21:]]
    for piece in pieces:
        _body(session, 0, piece)
        assert session.receive() == (_CREDIT, struct.pack("<QI", 0, len(piece)))
    _end(session, 0)
    assert session.receive() == (_LOADED, b"")

    assert session.evaluate("document.getElementById('text').textContent") == (False, '"café 𝄞"')
    assert session.evaluate("document.body.firstChild.nodeName") == (False, '"P"')
