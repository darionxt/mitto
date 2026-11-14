"""Tests of localStorage and sessionStorage in Pages that a Session opened, on local servers.

tests/storage_cases.js runs in a Page at an HTTP origin and gives what localStorage, sessionStorage and StorageEvent did,
which tests/data/storage_cases.json holds as Chrome 157 gave it for the same script on the same kind of page, headless on
macOS (157.0.8103.51). The other tests check what a Session keeps for its Pages: changes and storage events between
Pages of one storage key, as between a profile's tabs, sessionStorage for each Page, and a storage directory that a
Session in another process opens again, with the Machine it names.
"""

import dataclasses
import errno
import http.server
import json
import subprocess
import sys
import threading
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest

import mitto
from mitto import _native
from mitto._errors import ScriptError
from tests.local_server import QuietServer

_TIMEOUT_SECONDS = 60
_BLANK = b"<!doctype html><title>s</title><body>"
_TESTS = Path(__file__).parent
_CASES = (_TESTS / "storage_cases.js").read_text()
_AS_CHROME = json.loads((_TESTS / "data" / "storage_cases.json").read_text())
_SAVED_STORAGE_CLIENT = _TESTS / "saved_storage_client.py"

# What a page that listens for storage events keeps: each event's members, in order.
_LISTEN = """
  globalThis.heard = [];
  addEventListener('storage', (e) => heard.push([
    e.key, e.oldValue, e.newValue, e.url, e.storageArea === localStorage ? 'local'
        : e.storageArea === sessionStorage ? 'session' : String(e.storageArea),
    e.bubbles, e.cancelable, e.isTrusted]));
  localStorage.length;
  sessionStorage.length;
  'listening'
"""


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


def _serve_blank_page() -> tuple[QuietServer, str]:
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self) -> None:
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(_BLANK)))
            self.end_headers()
            self.wfile.write(_BLANK)

        def log_message(self, format: str, *args: Any) -> None:
            pass

    server = QuietServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, f"http://127.0.0.1:{server.server_port}/"


@pytest.fixture
def blank_page() -> Iterator[str]:
    """The URL of a blank page on 127.0.0.1."""
    server, url = _serve_blank_page()
    yield url
    server.shutdown()
    server.server_close()


@pytest.fixture
def other_blank_page() -> Iterator[str]:
    """The URL of a blank page of another origin than blank_page's: another port."""
    server, url = _serve_blank_page()
    yield url
    server.shutdown()
    server.server_close()


def _wait_for(page: Any, expression: str) -> Any:
    """Return what `expression` gives in `page` once it is truthy, waiting in the page for up to 10 seconds."""
    return page.evaluate(
        f"""
        new Promise((resolve, reject) => {{
          const started = performance.now();
          const look = () => {{
            const value = {expression};
            if (value) {{
              resolve(value);
            }} else if (performance.now() - started > 10000) {{
              reject(new Error('it never came'));
            }} else {{
              setTimeout(look, 10);
            }}
          }};
          look();
        }})
        """
    )


@pytest.mark.usefixtures("page_program")
def test_storage_is_chrome_s(blank_page: str) -> None:
    with mitto.Session() as session, session._open_page(blank_page, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        record = page.evaluate(_CASES)

    assert set(record) == set(_AS_CHROME["record"])
    for section, expected in _AS_CHROME["record"].items():
        assert record[section] == expected, section


@pytest.mark.usefixtures("page_program")
def test_a_page_of_an_opaque_origin_has_no_storage() -> None:
    script = """
      [localStorage, sessionStorage].map((s) => s)
    """
    with (
        mitto.Session() as session,
        session._open_page("about:blank", timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
        pytest.raises(ScriptError) as error,
    ):
        page.evaluate(script)

    assert str(error.value) == (
        "Uncaught SecurityError: Failed to read the 'localStorage' property from 'Window': "
        "Access is denied for this document."
    )


@pytest.mark.usefixtures("page_program")
def test_local_storage_reaches_the_session_s_other_pages_of_its_key(blank_page: str, other_blank_page: str) -> None:
    with (
        mitto.Session() as session,
        session._open_page(blank_page, timeout=_TIMEOUT_SECONDS) as writer,  # pyright: ignore[reportPrivateUsage]
        session._open_page(blank_page, timeout=_TIMEOUT_SECONDS) as same_origin,  # pyright: ignore[reportPrivateUsage]
        session._open_page(other_blank_page, timeout=_TIMEOUT_SECONDS) as other_origin,  # pyright: ignore[reportPrivateUsage]
    ):
        assert same_origin.evaluate(_LISTEN) == "listening"
        assert other_origin.evaluate(_LISTEN) == "listening"
        writer.evaluate(_LISTEN)
        writer.evaluate("localStorage.setItem('k', 'v1'); localStorage.setItem('k', 'v2'); localStorage.k = 'v2'")
        writer.evaluate("localStorage.removeItem('k'); localStorage.clear(); localStorage.setItem('other', 'x')")
        writer.evaluate("sessionStorage.setItem('k', 'session')")

        heard = _wait_for(same_origin, "heard.length >= 4 && heard")
        values = same_origin.evaluate("[localStorage.getItem('other'), localStorage.getItem('k'), localStorage.length]")
        assert same_origin.evaluate("sessionStorage.length") == 0
        # Nothing of another origin, and nothing of a Page's own changes.
        assert other_origin.evaluate("new Promise((r) => setTimeout(() => r(heard), 200))") == []
        assert other_origin.evaluate("localStorage.length") == 0
        assert writer.evaluate("heard") == []

    # The same value twice, and a clear() of an empty area, fire nothing.
    assert heard == [
        ["k", None, "v1", blank_page, "local", False, False, True],
        ["k", "v1", "v2", blank_page, "local", False, False, True],
        ["k", "v2", None, blank_page, "local", False, False, True],
        ["other", None, "x", blank_page, "local", False, False, True],
    ]
    assert values == ["x", None, 1]


@pytest.mark.usefixtures("page_program")
def test_a_clear_reaches_other_pages_once_and_only_when_the_area_held_something(blank_page: str) -> None:
    with (
        mitto.Session() as session,
        session._open_page(blank_page, timeout=_TIMEOUT_SECONDS) as writer,  # pyright: ignore[reportPrivateUsage]
        session._open_page(blank_page, timeout=_TIMEOUT_SECONDS) as listener,  # pyright: ignore[reportPrivateUsage]
    ):
        listener.evaluate(_LISTEN)
        writer.evaluate("localStorage.clear(); localStorage.a = '1'; localStorage.b = '2'; localStorage.clear()")
        heard = _wait_for(listener, "heard.length >= 3 && heard")

    assert heard == [
        ["a", None, "1", blank_page, "local", False, False, True],
        ["b", None, "2", blank_page, "local", False, False, True],
        [None, None, None, blank_page, "local", False, False, True],
    ]


@pytest.mark.usefixtures("page_program")
def test_session_storage_is_each_page_s_own(blank_page: str) -> None:
    with (
        mitto.Session() as session,
        session._open_page(blank_page, timeout=_TIMEOUT_SECONDS) as first,  # pyright: ignore[reportPrivateUsage]
        session._open_page(blank_page, timeout=_TIMEOUT_SECONDS) as second,  # pyright: ignore[reportPrivateUsage]
    ):
        second.evaluate(_LISTEN)
        first.evaluate("sessionStorage.setItem('k', 'first')")
        second.evaluate("sessionStorage.setItem('k', 'second')")

        assert first.evaluate("sessionStorage.getItem('k')") == "first"
        assert second.evaluate(
            "new Promise((r) => setTimeout(() => r([sessionStorage.getItem('k'), heard]), 200))"
        ) == [
            "second",
            [],
        ]


@pytest.mark.usefixtures("page_program")
def test_storage_in_memory_goes_with_its_session(blank_page: str) -> None:
    with mitto.Session() as session, session._open_page(blank_page, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        page.evaluate("localStorage.setItem('k', 'v')")
    with mitto.Session() as session, session._open_page(blank_page, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        assert page.evaluate("localStorage.getItem('k')") is None


@pytest.mark.usefixtures("page_program")
def test_a_storage_directory_brings_local_storage_back_in_another_process(blank_page: str, tmp_path: Path) -> None:
    storage = tmp_path / "storage"
    machine = mitto.Machine.draw(seed=11)
    with (
        mitto.Session(machine=machine, _storage=storage) as session,
        session._open_page(blank_page, timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
    ):
        page.evaluate("localStorage.setItem('kept', 'é and 日本'); sessionStorage.setItem('gone', 'x')")
        # A Page that closes before its Session does writes nothing that the Session does not.
        assert page.evaluate("localStorage.getItem('kept')") == "é and 日本"

    assert (storage / "LocalStorage").is_file()
    assert (storage / "Machine.json").is_file()
    script = "[localStorage.getItem('kept'), localStorage.length, sessionStorage.getItem('gone')]"
    completed = subprocess.run(
        [sys.executable, str(_SAVED_STORAGE_CLIENT), str(storage), blank_page, script],
        capture_output=True,
        text=True,
        timeout=_TIMEOUT_SECONDS * 2,
        check=True,
    )
    printed = json.loads(completed.stdout)

    assert printed["result"] == ["é and 日本", 1, None]
    assert printed["machine"] == dataclasses.asdict(machine)


@pytest.mark.usefixtures("page_program")
def test_storage_written_just_before_close_is_kept(blank_page: str, tmp_path: Path) -> None:
    storage = tmp_path / "storage"
    with (
        mitto.Session(_storage=storage) as session,
        session._open_page(blank_page, timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
    ):
        page.evaluate("for (let i = 0; i < 100; ++i) localStorage.setItem(`k${i}`, 'v'.repeat(i))")
    with (
        mitto.Session(_storage=storage) as session,
        session._open_page(blank_page, timeout=_TIMEOUT_SECONDS) as page,  # pyright: ignore[reportPrivateUsage]
    ):
        assert page.evaluate("[localStorage.length, localStorage.getItem('k99').length]") == [100, 99]


def test_a_storage_directory_names_the_machine_whose_storage_it_holds(native_library: Any, tmp_path: Path) -> None:
    storage = tmp_path / "storage"
    first = mitto.Machine.draw(seed=1)
    second = mitto.Machine.draw(seed=2)
    with mitto.Session(machine=first, _storage=storage):
        pass

    with mitto.Session(_storage=storage) as session:
        assert session.machine == first
    with pytest.raises(ValueError, match="holds the storage of another Machine than `machine` gives"):
        mitto.Session(machine=second, _storage=storage)
    cookie_file = tmp_path / "cookies.json"
    with mitto.Session(machine=second, cookie_file=cookie_file):
        pass
    with pytest.raises(ValueError, match=r"holds the cookies of another Machine than .* holds the storage of"):
        mitto.Session(cookie_file=cookie_file, _storage=storage)


def test_one_session_at_a_time_has_a_storage_directory(native_library: Any, tmp_path: Path) -> None:
    storage = tmp_path / "storage"
    with mitto.Session(_storage=storage):
        with pytest.raises(OSError, match="another Session has this storage directory open") as error:
            mitto.Session(_storage=storage)
        assert error.value.errno == errno.EBUSY
    with mitto.Session(_storage=storage):
        pass


def test_a_storage_directory_that_names_no_machine_is_refused(native_library: Any, tmp_path: Path) -> None:
    storage = tmp_path / "storage"
    storage.mkdir()
    (storage / "Machine.json").write_text('{"version": 1, "machine": {"memory": 8}}')

    with pytest.raises(ValueError, match="does not name a Machine that this Session can claim"):
        mitto.Session(_storage=storage)
