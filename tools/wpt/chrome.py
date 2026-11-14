"""Record Chrome's results for web-platform-tests: windowed Chrome of the Engine Release, as a Baseline records it.

Chrome starts from a new user data directory, waits for the variations seed and closes, as `tools/baselines/record.ps1`
does. Each window of a recording is then a Chrome of its own, started from a copy of that directory with the flags of a
case page, so that tests in different windows share no cookies or storage, and each runs its tests one after another, as
wptrunner runs a browser. Each test opens in a new window, as wptrunner gives each test a window, through the browser's
debugging port. The windows are tiled, so that none covers another: Chrome hides a window that something covers, and a
hidden page runs its timers late and no animation frames.
"""

import contextlib
import itertools
import json
import math
import queue
import shutil
import subprocess
import time
import urllib.request
from collections.abc import Iterable, Iterator, Sequence
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path
from types import TracebackType
from typing import Any, Self, cast

from websockets.sync.client import ClientConnection, connect

from tools.wpt.results import TIMEOUT, ResultsError, RunResult, result_from_record
from tools.wpt.server import HOST_RESOLVER_RULES, SPKI, WptServer

__all__ = ["DEBUGGING_PORT", "Chrome", "ChromeError", "Recording", "record", "wait_for_seed"]

DEBUGGING_PORT = 9207
"""The debugging port of the first Chrome, and the next ones count up from it. A fixed one, since port 0 makes
navigator.webdriver true, as baselines.md says."""

_COMMON_FLAGS = ("--no-first-run", "--no-default-browser-check", "--lang=en-US", "--variations-override-country=us")
"""What every start of Chrome for a Baseline has, with its user data directory."""

_RECORDING_FLAGS = (
    f"--ignore-certificate-errors-spki-list={SPKI}",
    f"--host-resolver-rules={HOST_RESOLVER_RULES}",
    "--no-proxy-server",
)
"""What a start at a case page adds: the server's certificate and names, and no proxy."""

_NOT_COPIED = shutil.ignore_patterns("Singleton*", "lockfile", "DevToolsActivePort")
"""What a running Chrome leaves in its user data directory that a copy must not take."""

_SEED_SECONDS = 360.0
_START_SECONDS = 60.0
_STOP_SECONDS = 30.0
_ANSWER_SECONDS = 30.0
_POLL_SECONDS = 0.05
_READ_SECONDS = 5.0
_PROGRESS_EVERY = 500
_HIDDEN_RETRIES = 2
"""How many times a test that reported from a hidden page runs again, in whichever window is free."""


class ChromeError(Exception):
    """Chrome did not start, did not get its variations seed, or did not answer."""


def wait_for_seed(executable: Path, user_data_directory: Path) -> str:
    """Start Chrome from a new user data directory, wait until it has stored a variations seed, and close it.

    Returns:
        The seed's serial number, as `Local State` holds it, or "" if only the country came.

    Raises:
        ChromeError: No seed arrived within six minutes, or Chrome did not start or stop.
    """
    with Chrome(executable, user_data_directory, recording=False):
        deadline = time.monotonic() + _SEED_SECONDS
        while time.monotonic() < deadline:
            state = _local_state(user_data_directory)
            if "variations_seed_serial_number" in state or "variations_country" in state:
                return str(state.get("variations_seed_serial_number") or "")
            time.sleep(1)
    raise ChromeError(f"no variations seed arrived in {_SEED_SECONDS:g} seconds")


def _local_state(user_data_directory: Path) -> dict[str, Any]:
    try:
        return cast(dict[str, Any], json.loads((user_data_directory / "Local State").read_text(encoding="utf-8")))
    except (OSError, ValueError):
        return {}


@dataclass(frozen=True, slots=True)
class Tile:
    """Where a window goes on the screen, in CSS pixels."""

    left: int
    top: int
    width: int
    height: int


class Chrome:
    """Windowed Chrome with a debugging port, which it closes as a person would. Use it in a with statement."""

    def __init__(
        self, executable: Path, user_data_directory: Path, *, port: int = DEBUGGING_PORT, recording: bool = True
    ) -> None:
        """Make it.

        Args:
            executable: chrome.exe of the Engine Release.
            user_data_directory: Its user data directory.
            port: Its debugging port.
            recording: Whether to start it with the flags of a case page, rather than those of the start that fetches
                the seed.
        """
        self._executable = executable
        self._port = port
        self._flags = [
            f"--user-data-dir={user_data_directory}",
            *_COMMON_FLAGS,
            *(_RECORDING_FLAGS if recording else ()),
            f"--remote-debugging-port={port}",
        ]
        self._process: subprocess.Popen[bytes] | None = None
        self._browser_socket = ""
        self.version = ""

    def __enter__(self) -> Self:
        self._process = subprocess.Popen(
            [str(self._executable), *self._flags, "about:blank"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            self._browser_socket = self._wait_for_socket()
            with self._connection() as browser:
                self.version = str(_call(browser, itertools.count(1), "Browser.getVersion")["product"])
        except BaseException:
            self._process.kill()
            self._process.wait()
            self._process = None
            raise
        return self

    def __exit__(
        self, exc_type: type[BaseException] | None, exc: BaseException | None, traceback: TracebackType | None
    ) -> None:
        process, self._process = self._process, None
        if process is None:
            return
        with contextlib.suppress(OSError, ChromeError), self._connection() as browser:
            _call(browser, itertools.count(1), "Browser.close")
        try:
            process.wait(_STOP_SECONDS)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise ChromeError("Chrome did not close") from None

    def _wait_for_socket(self) -> str:
        """Ask the debugging port for the browser's address, once Chrome answers there."""
        deadline = time.monotonic() + _START_SECONDS
        while time.monotonic() < deadline:
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{self._port}/json/version", timeout=5) as answer:
                    return str(json.loads(answer.read())["webSocketDebuggerUrl"])
            except (OSError, ValueError, KeyError):
                time.sleep(_POLL_SECONDS)
        raise ChromeError(f"Chrome did not answer on its debugging port in {_START_SECONDS:g} seconds")

    def _connection(self) -> ClientConnection:
        return connect(self._browser_socket, proxy=None, max_size=None, open_timeout=_ANSWER_SECONDS)

    def minimize_windows(self) -> None:
        """Minimize the windows that Chrome has open, such as the one that it starts with, so that none covers a test's
        window: Windows opens a window of a process in the background behind the windows in front."""
        with self._connection() as browser:
            calls = itertools.count(1)
            targets = cast(list[dict[str, Any]], _call(browser, calls, "Target.getTargets")["targetInfos"])
            for target in targets:
                if target.get("type") == "page":
                    window = _call(browser, calls, "Browser.getWindowForTarget", targetId=target["targetId"])[
                        "windowId"
                    ]
                    _call(
                        browser, calls, "Browser.setWindowBounds", windowId=window, bounds={"windowState": "minimized"}
                    )

    def screen(self) -> Tile:
        """The screen that windows can take, as a page sees it."""
        with self._connection() as browser:
            calls = itertools.count(1)
            target = str(_call(browser, calls, "Target.createTarget", url="about:blank", newWindow=True)["targetId"])
            try:
                area = _evaluate(
                    browser, calls, target, "[screen.availLeft, screen.availTop, screen.availWidth, screen.availHeight]"
                )
            finally:
                _call(browser, calls, "Target.closeTarget", targetId=target)
        return Tile(*cast(Sequence[int], area))

    def run_in_window(self, server: WptServer, test: str, seconds: float, tile: Tile) -> tuple[RunResult, str | None]:
        """Run one test in a new window at a tile, and return its result, with the page's visibility when it reported,
        which is "visible" unless something hid the window.

        Raises:
            ChromeError: Chrome did not open the window.
        """
        server.forget(test)
        with self._connection() as browser:
            calls = itertools.count(1)
            deadline = time.monotonic() + seconds
            target = str(
                _call(
                    browser,
                    calls,
                    "Target.createTarget",
                    url=server.url_for(test),
                    newWindow=True,
                    left=tile.left,
                    top=tile.top,
                    width=tile.width,
                    height=tile.height,
                )["targetId"]
            )
            try:
                while (record := server.posted(test)) is None and time.monotonic() < deadline:
                    time.sleep(_POLL_SECONDS)
                if record is None:
                    record = _read_results(browser, calls, target)
            finally:
                # A test may have closed its window itself.
                with contextlib.suppress(ChromeError):
                    _call(browser, calls, "Target.closeTarget", targetId=target)
        if record is None:
            return RunResult(test, TIMEOUT), None
        try:
            return result_from_record(test, record), cast(str | None, record.get("visibility"))
        except ResultsError as error:
            return RunResult(test, "ERROR", str(error)), None


@dataclass(slots=True)
class Recording:
    """What one recording gave.

    Attributes:
        version: Chrome's product and build, such as "Chrome/157.0.8103.39".
        seed: The serial number of its variations seed.
        results: Each test's result, in no order.
        hidden: The tests that reported from a page that was not visible.
    """

    version: str = ""
    seed: str = ""
    results: list[RunResult] = field(default_factory=list[RunResult])
    hidden: list[str] = field(default_factory=list[str])


def record(executable: Path, directory: Path, tests: Iterable[tuple[str, float]], windows: int) -> Recording:
    """Record Chrome's results for tests, each with how many seconds it has, in `windows` Chromes at once.

    `directory` must be new: it gets the user data directory that fetches the seed, `seed`, and a copy of it for each
    window, named by its number. It prints how far it has got.

    Raises:
        ChromeError: Chrome got no seed, did not start, or did not open a window.
        ServerError: The server did not start.
    """
    recording = Recording()
    recording.seed = wait_for_seed(executable, directory / "seed")
    copies = [directory / str(number) for number in range(1, windows + 1)]
    for copy in copies:
        shutil.copytree(directory / "seed", copy, ignore=_NOT_COPIED)
    plan: queue.SimpleQueue[tuple[str, float]] = queue.SimpleQueue()
    for test in tests:
        plan.put(test)
    total = plan.qsize()
    started = time.monotonic()
    with WptServer() as server, contextlib.ExitStack() as stack:
        chromes = [
            stack.enter_context(Chrome(executable, copy, port=DEBUGGING_PORT + index))
            for index, copy in enumerate(copies)
        ]
        for chrome in chromes:
            chrome.minimize_windows()
        recording.version = chromes[0].version
        print(f"{recording.version}, seed {recording.seed}, {total} tests, {windows} windows.", flush=True)
        tiles = _tiles(chromes[0].screen(), windows)

        hidden_runs: dict[str, int] = {}

        def work(chrome: Chrome, tile: Tile) -> None:
            while True:
                try:
                    test, seconds = plan.get_nowait()
                except queue.Empty:
                    return
                result, visibility = chrome.run_in_window(server, test, seconds, tile)
                if visibility not in (None, "visible"):
                    hidden_runs[test] = hidden_runs.get(test, 0) + 1
                    if hidden_runs[test] <= _HIDDEN_RETRIES:
                        plan.put((test, seconds))
                        continue
                    recording.hidden.append(test)
                recording.results.append(result)
                if len(recording.results) % _PROGRESS_EVERY == 0:
                    print(
                        f"{len(recording.results)} of {total} tests, {time.monotonic() - started:.0f} seconds",
                        flush=True,
                    )

        with ThreadPoolExecutor(windows) as pool:
            for future in [pool.submit(work, chrome, tile) for chrome, tile in zip(chromes, tiles, strict=True)]:
                future.result()
    print(f"Recorded {len(recording.results)} tests in {time.monotonic() - started:.0f} seconds.", flush=True)
    return recording


def _tiles(screen: Tile, count: int) -> list[Tile]:
    columns = math.ceil(math.sqrt(count))
    rows = math.ceil(count / columns)
    width, height = screen.width // columns, screen.height // rows
    return [
        Tile(screen.left + (index % columns) * width, screen.top + (index // columns) * height, width, height)
        for index in range(count)
    ]


def _read_results(browser: ClientConnection, calls: Iterator[int], target: str) -> dict[str, Any] | None:
    """Read the results that a page could not post, as the reporter keeps them, through the debugging port."""
    try:
        text = _evaluate(browser, calls, target, "self.__mittoResults ?? null", seconds=_READ_SECONDS)
    except ChromeError:
        return None
    return cast(dict[str, Any], json.loads(text)) if isinstance(text, str) else None


def _evaluate(
    browser: ClientConnection, calls: Iterator[int], target: str, expression: str, seconds: float = _ANSWER_SECONDS
) -> object:
    session = str(_call(browser, calls, "Target.attachToTarget", targetId=target, flatten=True)["sessionId"])
    answer = _call(
        browser, calls, "Runtime.evaluate", session=session, seconds=seconds, expression=expression, returnByValue=True
    )
    return cast(dict[str, Any], answer.get("result", {})).get("value")


def _call(
    browser: ClientConnection,
    calls: Iterator[int],
    method: str,
    *,
    session: str | None = None,
    seconds: float = _ANSWER_SECONDS,
    **params: object,
) -> dict[str, Any]:
    call = next(calls)
    command: dict[str, object] = {"id": call, "method": method, "params": params}
    if session is not None:
        command["sessionId"] = session
    browser.send(json.dumps(command))
    deadline = time.monotonic() + seconds
    while (remaining := deadline - time.monotonic()) > 0:
        try:
            answer = cast(dict[str, Any], json.loads(browser.recv(remaining)))
        except TimeoutError:
            break
        if answer.get("id") == call:
            if "error" in answer:
                raise ChromeError(f"{method}: {answer['error']}")
            return cast(dict[str, Any], answer.get("result", {}))
    raise ChromeError(f"{method}: no answer in {seconds:g} seconds")
