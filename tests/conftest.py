"""Fixtures shared by the tests of the native core's binding and of the public API."""

import ctypes
import faulthandler
import os
import subprocess
import sys
import threading
from collections.abc import Iterator
from functools import partial
from typing import Any

import pytest

import mitto
from mitto import _api, _native
from tests.session_on_asyncio import SessionOnAsyncio

# The copy of web-platform-tests holds no tests of pytest's, and 11,000 files to walk.
collect_ignore = ["wpt"]

# How long a test runs before it says what it waits on: longer than any test takes, the parity tests included.
_STUCK_AFTER_SECONDS = 600.0


def pytest_collection_modifyitems(items: list[pytest.Item]) -> None:
    """Skip the tests marked alone in a worker of pytest-xdist that has other workers beside it."""
    if int(os.environ.get("PYTEST_XDIST_WORKER_COUNT", "1")) <= 1:
        return
    skip = pytest.mark.skip(reason="times something by the wall clock: run it in a pytest without -n, as CI does")
    for item in items:
        if item.get_closest_marker("alone") is not None:
            item.add_marker(skip)


@pytest.fixture(autouse=True)
def report_a_stuck_test(request: pytest.FixtureRequest) -> Iterator[None]:
    """Have a test that runs past _STUCK_AFTER_SECONDS write its name, each thread's stack and, on macOS, a sample of each
    of its Page processes to the run's standard error, past pytest's capture, so that a run that hangs says where."""
    capture = request.config.pluginmanager.getplugin("capturemanager")
    timer = threading.Timer(_STUCK_AFTER_SECONDS, _report_stuck, args=(request.node.nodeid, capture))
    timer.daemon = True
    timer.start()
    yield
    timer.cancel()


def _report_stuck(nodeid: str, capture: Any) -> None:
    with capture.global_and_fixture_disabled():
        print(f"\n{nodeid} has run for {_STUCK_AFTER_SECONDS:.0f} s; its threads:", file=sys.stderr, flush=True)
        faulthandler.dump_traceback(file=sys.stderr, all_threads=True)
        if sys.platform != "darwin":
            return
        pages = subprocess.run(
            ["pgrep", "-P", str(os.getpid()), "-x", "mitto_page"], capture_output=True, text=True, check=False
        ).stdout.split()
        for pid in pages:
            print(f"\nmitto_page {pid} of {nodeid}:", file=sys.stderr, flush=True)
            subprocess.run(["sample", pid, "3"], stdout=sys.stderr, stderr=subprocess.STDOUT, check=False)


@pytest.fixture(autouse=True)
def fresh_load_library() -> Iterator[None]:
    """Make each test load the library and create the default Session for itself."""
    _native.load_library.cache_clear()
    _api.default_session.cache_clear()
    yield
    _native.load_library.cache_clear()
    _api.default_session.cache_clear()


@pytest.fixture
def native_library() -> ctypes.CDLL:
    """Return the real native core, or skip if this machine has none."""
    if _native.find_library() is None:
        pytest.skip(f"no native library; set {_native.LIBRARY_ENV_VAR} to run this test")
    return _native.load_library()


@pytest.fixture(params=["sync", "asyncio"])
def both_apis(request: pytest.FixtureRequest, monkeypatch: pytest.MonkeyPatch) -> Iterator[str]:
    """Run a test once on mitto.Session, and once with mitto.Session being a mitto.AsyncSession behind blocking calls.

    A feature test says what a Session does for its caller, so it holds for both. A module of them asks for this in its
    pytestmark, and what its tests make through mitto.Session then goes through each API in turn. mitto.get() and the
    native Session are not part of this.
    """
    created: list[SessionOnAsyncio] = []
    if request.param == "asyncio":
        monkeypatch.setattr(mitto, "Session", partial(SessionOnAsyncio, created.append))
    yield request.param
    for session in created:
        session.end_of_test()
