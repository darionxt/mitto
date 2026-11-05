"""Fixtures shared by the tests of the native core's binding and of the public API."""

import ctypes
from collections.abc import Iterator

import pytest

from mitto import _api, _native


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
