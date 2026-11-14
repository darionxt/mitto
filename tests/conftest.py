"""Fixtures shared by the tests of the native core's binding and of the public API."""

import ctypes
import os
from collections.abc import Iterator
from functools import partial

import pytest

import mitto
from mitto import _api, _native
from tests.session_on_asyncio import SessionOnAsyncio


def pytest_collection_modifyitems(items: list[pytest.Item]) -> None:
    """Skip the tests marked alone in a worker of pytest-xdist that has other workers beside it."""
    if int(os.environ.get("PYTEST_XDIST_WORKER_COUNT", "1")) <= 1:
        return
    skip = pytest.mark.skip(reason="times something by the wall clock: run it in a pytest without -n, as CI does")
    for item in items:
        if item.get_closest_marker("alone") is not None:
            item.add_marker(skip)


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
