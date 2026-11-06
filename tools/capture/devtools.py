"""Clicks what the case pages ask to have clicked, in a Chrome that has a debugging port.

A navigation counts as the user's only with a user gesture, and with a gesture Chrome sends Sec-Fetch-User. A script on
the page has none to give, so the gesture has to come from outside. It goes through the debugging port, and only runs
the page's own click on the page's own element.
"""

import contextlib
import http.client
import itertools
import json
import threading
from pathlib import Path
from types import TracebackType
from typing import Self, cast

from websockets.exceptions import WebSocketException
from websockets.sync.client import connect

_ACTIVE_PORT_FILE = "DevToolsActivePort"
_LOOPBACK = "127.0.0.1"
_POLL_SECONDS = 0.1
_ANSWER_TIMEOUT_SECONDS = 5.0
_STOP_TIMEOUT_SECONDS = 10.0
_WEB_SCHEMES = ("http://", "https://")

_CLICK_NEXT = """
(() => {
  const next = document.getElementById("next");
  if (!next || next.hidden || next.dataset.clicked) return false;
  next.dataset.clicked = "true";
  next.click();
  return true;
})()
"""


class PageClicker:
    """While its context lasts, clicks each page's next element, when there is one, in the Chrome of a user data directory.

    It keeps trying, because Chrome starts, navigates and goes away in its own time. A page that never gets its click
    shows as a page that never finishes.
    """

    def __init__(self, user_data_directory: Path) -> None:
        self._port_file = user_data_directory / _ACTIVE_PORT_FILE
        self._stopping = threading.Event()
        self._thread = threading.Thread(target=self._loop, daemon=True)

    def __enter__(self) -> Self:
        self._thread.start()
        return self

    def __exit__(
        self, exc_type: type[BaseException] | None, exc: BaseException | None, traceback: TracebackType | None
    ) -> None:
        self._stopping.set()
        self._thread.join(_STOP_TIMEOUT_SECONDS)

    def _loop(self) -> None:
        while not self._stopping.wait(_POLL_SECONDS):
            with contextlib.suppress(
                OSError, ValueError, LookupError, StopIteration, http.client.HTTPException, WebSocketException
            ):
                self._drive_page()

    def _drive_page(self) -> None:
        with connect(self._page_socket(), proxy=None, open_timeout=_ANSWER_TIMEOUT_SECONDS) as page:
            for call in itertools.count(1):
                if self._stopping.is_set():
                    return
                command = {
                    "id": call,
                    "method": "Runtime.evaluate",
                    "params": {"expression": _CLICK_NEXT, "userGesture": True},
                }
                page.send(json.dumps(command))
                while json.loads(page.recv(_ANSWER_TIMEOUT_SECONDS)).get("id") != call:
                    pass
                self._stopping.wait(_POLL_SECONDS)

    def _page_socket(self) -> str:
        """Return the debugging address of the page that shows a web address."""
        port = int(self._port_file.read_text(encoding="utf-8").split()[0])
        connection = http.client.HTTPConnection(_LOOPBACK, port, timeout=_ANSWER_TIMEOUT_SECONDS)
        try:
            connection.request("GET", "/json/list")
            targets = cast(list[dict[str, str]], json.loads(connection.getresponse().read()))
        finally:
            connection.close()
        return next(
            target["webSocketDebuggerUrl"]
            for target in targets
            if target["type"] == "page" and target["url"].startswith(_WEB_SCHEMES)
        )
