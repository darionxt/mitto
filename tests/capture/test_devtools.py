"""Tests of what clicks the pages, against a stand-in for Chrome's debugging port."""

import json
import threading
import time
from collections.abc import Callable, Iterator
from http import HTTPStatus
from pathlib import Path
from typing import Any

import pytest
from websockets.http11 import Request, Response
from websockets.sync.server import Server, ServerConnection, serve

from tools.capture.devtools import PageClicker

_TIMEOUT_SECONDS = 10.0

# A clicker that dies is a page that never finishes, for no reason anyone could see.
pytestmark = pytest.mark.filterwarnings("error::pytest.PytestUnhandledThreadExceptionWarning")


class FakeChrome:
    """Answers /json/list, takes commands on its one page, and notes them. Each command also gets an event first."""

    def __init__(self, targets: list[dict[str, str]] | None = None) -> None:
        self.commands: list[dict[str, Any]] = []
        self.server: Server = serve(self._serve_page, "127.0.0.1", 0, process_request=self._serve_list)
        self.port: int = self.server.socket.getsockname()[1]
        self.targets = targets
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self) -> None:
        self.server.shutdown()
        self.thread.join(_TIMEOUT_SECONDS)

    def _serve_list(self, connection: ServerConnection, request: Request) -> Response | None:
        if request.path != "/json/list":
            return None
        page = {"type": "page", "url": "https://capture.test/", "webSocketDebuggerUrl": f"ws://127.0.0.1:{self.port}/p"}
        return connection.respond(HTTPStatus.OK, json.dumps([page] if self.targets is None else self.targets))

    def _serve_page(self, connection: ServerConnection) -> None:
        for message in connection:
            command = json.loads(message)
            self.commands.append(command)
            connection.send(json.dumps({"method": "Some.event"}))
            connection.send(json.dumps({"id": command["id"], "result": {}}))


@pytest.fixture
def fake_chrome() -> Iterator[FakeChrome]:
    chrome = FakeChrome()
    yield chrome
    chrome.close()


def wait_for(condition: Callable[[], object], what: str) -> None:
    deadline = time.monotonic() + _TIMEOUT_SECONDS
    while not condition():
        assert time.monotonic() < deadline, what
        time.sleep(0.01)


def test_clicks_with_a_user_gesture_and_keeps_at_it(tmp_path: Path, fake_chrome: FakeChrome) -> None:
    (tmp_path / "DevToolsActivePort").write_text(f"{fake_chrome.port}\n/devtools/browser/x\n", encoding="utf-8")

    with PageClicker(tmp_path):
        wait_for(lambda: len(fake_chrome.commands) >= 2, "no second command")

    command = fake_chrome.commands[0]
    assert command["method"] == "Runtime.evaluate"
    assert command["params"]["userGesture"] is True
    assert 'getElementById("next")' in command["params"]["expression"]


def test_waits_for_chrome_to_say_where_its_port_is(tmp_path: Path, fake_chrome: FakeChrome) -> None:
    with PageClicker(tmp_path):
        time.sleep(0.3)
        assert fake_chrome.commands == []
        (tmp_path / "DevToolsActivePort").write_text(f"{fake_chrome.port}\n", encoding="utf-8")
        wait_for(lambda: fake_chrome.commands, "no command")


@pytest.mark.parametrize("text", ["", "not a number\n", "1\n"])
def test_survives_a_port_file_that_leads_nowhere(tmp_path: Path, text: str) -> None:
    (tmp_path / "DevToolsActivePort").write_text(text, encoding="utf-8")

    with PageClicker(tmp_path):
        time.sleep(0.3)


def test_leaves_alone_a_page_that_shows_no_web_address(tmp_path: Path) -> None:
    other = {"type": "page", "url": "chrome://version/", "webSocketDebuggerUrl": "ws://127.0.0.1:1/x"}
    worker = {"type": "service_worker", "url": "https://capture.test/", "webSocketDebuggerUrl": "ws://127.0.0.1:1/y"}
    chrome = FakeChrome([other, worker])
    (tmp_path / "DevToolsActivePort").write_text(f"{chrome.port}\n", encoding="utf-8")
    try:
        with PageClicker(tmp_path):
            time.sleep(0.3)
    finally:
        chrome.close()

    assert chrome.commands == []
