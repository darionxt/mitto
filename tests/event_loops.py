"""The ways an event loop can hear of an AsyncSession's events, which the tests of the asyncio API go through in turn.

On macOS and Linux the loop watches the Session's file descriptor, and a test can have a thread wait for the events
instead, as on Windows. On Windows the thread is the only way, and it hands them to either of its event loops. The feature
tests, which go through mitto.AsyncSession by the both_apis fixture, take the Host's own way on its default event loop.
"""

import asyncio
import sys
from collections.abc import Callable

import pytest

from mitto import _async_api

__all__ = ["KINDS", "loop_class_name", "new_event_loop", "use", "waits_on_a_thread"]

type LoopFactory = Callable[[], asyncio.AbstractEventLoop]

# Each way, by the name that a test's id shows, with the name in asyncio of the class of its event loop.
if sys.platform == "win32":
    _LOOP_CLASSES = {"proactor": "ProactorEventLoop", "selector": "SelectorEventLoop"}
else:
    _LOOP_CLASSES = {"fd": "SelectorEventLoop", "thread": "SelectorEventLoop"}

KINDS = tuple(_LOOP_CLASSES)


class _Chosen:
    loop_factory: LoopFactory = asyncio.new_event_loop


_chosen = _Chosen()


def waits_on_a_thread(kind: str) -> bool:
    """Return whether a thread that waits in the native core tells the loop of events, for the way `kind` names."""
    return kind != "fd"


def use(kind: str, monkeypatch: pytest.MonkeyPatch) -> None:
    """Make an AsyncSession that opens in this test hear of its events as `kind` says, on a loop from new_event_loop()."""
    monkeypatch.setattr(_async_api, "_WAIT_ON_A_THREAD", waits_on_a_thread(kind))
    loop_factory: LoopFactory = getattr(asyncio, _LOOP_CLASSES[kind])
    monkeypatch.setattr(_chosen, "loop_factory", loop_factory)


def loop_class_name(kind: str) -> str:
    """Return the name in asyncio of the class of event loop for `kind`, for a process of its own to make one."""
    return _LOOP_CLASSES[kind]


def new_event_loop() -> asyncio.AbstractEventLoop:
    """Return a new event loop of the kind that use() chose, or else of the default kind."""
    return _chosen.loop_factory()
