"""Tests of a Session's HTTP cache, through the public API. They need a native library.

//net decides what a cache does, as it does in Chrome, so these check what a caller and a server can see of that, and what
a Session itself adds: where the cache lives, who may use it, and which site a request belongs to.
"""

import concurrent.futures
import errno
import gzip
import http.server
import json
import os
import subprocess
import sys
import textwrap
import threading
import time
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path
from typing import Any, cast
from urllib.parse import urlsplit

import pytest

import mitto
from mitto import _native
from mitto._native import PrivateSettings
from tests.local_server import QuietServer
from tools.capture import HTTP1, HTTP2, HTTP3, CaptureServer
from tools.parity.replay import run_of, session_settings

pytestmark = pytest.mark.usefixtures("native_library")

ETAG = '"version-1"'
LAST_MODIFIED = "Wed, 01 Jan 2025 00:00:00 GMT"
BIG_BODY = bytes(range(256)) * 4096  # 1 MiB, past what one read holds and what the backend keeps in a block.
GZIPPED_TEXT = b"text that went out compressed"

PAGE = "http://www.example.test/page"
PAGE_OF_ANOTHER_SITE = "http://www.other.test/page"
PAGE_ON_THE_SAME_SITE = "http://other.example.test/page"
TARGET_HOST = "www.third.test"
ALL_NAMES = PrivateSettings(host_resolver_rules="MAP * 127.0.0.1")


@dataclass(frozen=True, slots=True)
class Received:
    method: str
    path: str
    headers: dict[str, str]


class CacheServer(QuietServer):
    """Answers each path in the way that its name says, and numbers its answers, so a test can tell one from another."""

    def __init__(self) -> None:
        super().__init__(("127.0.0.1", 0), CacheHandler)
        self.received: list[Received] = []

    def url(self, path: str, host: str = "127.0.0.1") -> str:
        return f"http://{host}:{self.server_address[1]}{path}"

    def paths(self) -> list[str]:
        return [request.path for request in self.received]

    def requests_to(self, path: str) -> list[Received]:
        return [request for request in self.received if request.path == path]


class CacheHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def answer(self) -> None:
        server = cast(CacheServer, self.server)
        self.rfile.read(int(self.headers.get("Content-Length", 0)))
        server.received.append(
            Received(self.command, self.path, {name.lower(): value for name, value in self.headers.items()})
        )
        body = b"answer %d" % len(server.received)
        match urlsplit(self.path).path:
            case "/fresh":
                self.respond(200, body, ("Cache-Control", "max-age=3600"), ("X-Answer", body.decode()))
            case "/no-store":
                self.respond(200, body, ("Cache-Control", "no-store"))
            case "/slow":
                time.sleep(0.2)
                self.respond(200, body, ("Cache-Control", "max-age=3600"))
            case "/etag" if self.headers.get("If-None-Match") == ETAG:
                self.respond(304, b"", ("ETag", ETAG), ("Cache-Control", "no-cache"), ("X-Validated", "yes"))
            case "/etag":
                self.respond(200, body, ("ETag", ETAG), ("Cache-Control", "no-cache"))
            case "/changing-etag":
                self.respond(200, body, ("ETag", f'"{len(server.received)}"'), ("Cache-Control", "no-cache"))
            case "/expired-etag" if self.headers.get("If-None-Match") == ETAG:
                self.respond(304, b"", ("ETag", ETAG), ("Cache-Control", "max-age=0"))
            case "/expired-etag":
                self.respond(200, body, ("ETag", ETAG), ("Cache-Control", "max-age=0"))
            case "/last-modified" if self.headers.get("If-Modified-Since") == LAST_MODIFIED:
                self.respond(304, b"", ("Last-Modified", LAST_MODIFIED), ("Cache-Control", "no-cache"))
            case "/last-modified":
                self.respond(200, body, ("Last-Modified", LAST_MODIFIED), ("Cache-Control", "no-cache"))
            case "/vary":
                self.respond(200, body, ("Cache-Control", "max-age=3600"), ("Vary", "X-Choice"))
            case "/moved":
                self.respond(301, b"", ("Location", "/fresh"), ("Cache-Control", "max-age=3600"))
            case "/gzip":
                self.respond(
                    200,
                    gzip.compress(GZIPPED_TEXT),
                    ("Cache-Control", "max-age=3600"),
                    ("Content-Encoding", "gzip"),
                )
            case "/big":
                self.respond(200, BIG_BODY, ("Cache-Control", "max-age=3600"))
            case "/refused":
                self.respond(500, body)
            case _:
                self.respond(200, body)

    def respond(self, status: int, body: bytes, *headers: tuple[str, str]) -> None:
        self.send_response(status)
        for name, value in headers:
            self.send_header(name, value)
        self.send_header("Content-Type", "text/plain")
        if status != 304:
            self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD" and status != 304:
            self.wfile.write(body)

    def do_GET(self) -> None:
        self.answer()

    def do_HEAD(self) -> None:
        self.answer()

    def do_POST(self) -> None:
        self.answer()

    def do_PUT(self) -> None:
        self.answer()

    def do_PATCH(self) -> None:
        self.answer()

    def do_DELETE(self) -> None:
        self.answer()

    def log_message(self, format: str, *args: Any) -> None:
        pass


@pytest.fixture
def server() -> Iterator[CacheServer]:
    with CacheServer() as running:
        thread = threading.Thread(target=running.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
        thread.start()
        yield running
        running.shutdown()
        thread.join()


@pytest.fixture(params=["memory", "directory"])
def cache(request: pytest.FixtureRequest, tmp_path: Path) -> str | Path:
    """A cache of each kind, for what they have in common."""
    return "memory" if request.param == "memory" else tmp_path / "cache"


def test_a_session_keeps_no_responses_by_default(server: CacheServer) -> None:
    url = server.url("/etag")

    with mitto.Session() as session:
        first = session.get(url)
        second = session.get(url)

    assert server.paths() == ["/etag", "/etag"]
    assert "if-none-match" not in server.received[1].headers
    assert (first.from_cache, second.from_cache) == (False, False)
    assert second.text == "answer 2"


def test_a_second_get_reaches_no_server(server: CacheServer, cache: str | Path) -> None:
    url = server.url("/fresh")

    with mitto.Session(cache=cache) as session:
        first = session.get(url)
        second = session.get(url)

    assert server.paths() == ["/fresh"]
    assert first.from_cache is False
    assert second.from_cache is True
    assert second.text == first.text == "answer 1"


def test_a_response_from_the_cache_says_what_the_first_one_said(server: CacheServer, cache: str | Path) -> None:
    url = server.url("/fresh")

    with mitto.Session(cache=cache) as session:
        first = session.get(url)
        second = session.get(url)

    assert second.status == first.status == 200
    assert second.url == first.url == url
    assert second.protocol == first.protocol == "http/1.1"
    assert second.headers["X-Answer"] == "answer 1"
    assert [name for name, _ in second.headers] == [name for name, _ in first.headers]


def test_a_revalidation_asks_with_the_etag_and_answers_from_the_cache(server: CacheServer, cache: str | Path) -> None:
    url = server.url("/etag")

    with mitto.Session(cache=cache) as session:
        first = session.get(url)
        second = session.get(url)

    assert server.paths() == ["/etag", "/etag"]
    assert "if-none-match" not in server.received[0].headers
    assert server.received[1].headers["if-none-match"] == ETAG
    assert first.from_cache is False
    assert second.from_cache is True
    assert second.status == 200
    assert second.text == "answer 1"


def test_a_revalidation_takes_the_new_headers_of_a_304(server: CacheServer, cache: str | Path) -> None:
    url = server.url("/etag")

    with mitto.Session(cache=cache) as session:
        first = session.get(url)
        second = session.get(url)

    assert "X-Validated" not in first.headers
    assert second.headers["X-Validated"] == "yes"


def test_a_revalidation_asks_with_the_date_when_there_is_no_etag(server: CacheServer, cache: str | Path) -> None:
    url = server.url("/last-modified")

    with mitto.Session(cache=cache) as session:
        session.get(url)
        second = session.get(url)

    assert server.received[1].headers["if-modified-since"] == LAST_MODIFIED
    assert second.from_cache is True


def test_a_stale_response_is_revalidated(server: CacheServer, cache: str | Path) -> None:
    url = server.url("/expired-etag")

    with mitto.Session(cache=cache) as session:
        session.get(url)
        second = session.get(url)

    assert server.received[1].headers["if-none-match"] == ETAG
    assert second.from_cache is True


def test_a_new_version_replaces_the_one_in_the_cache(server: CacheServer, cache: str | Path) -> None:
    url = server.url("/changing-etag")

    with mitto.Session(cache=cache) as session:
        session.get(url)
        second = session.get(url)
        session.get(url)

    assert second.from_cache is False
    assert second.text == "answer 2"
    assert server.received[1].headers["if-none-match"] == '"1"'
    assert server.received[2].headers["if-none-match"] == '"2"'


def test_no_store_is_not_kept(server: CacheServer, cache: str | Path) -> None:
    url = server.url("/no-store")

    with mitto.Session(cache=cache) as session:
        session.get(url)
        second = session.get(url)

    assert server.paths() == ["/no-store", "/no-store"]
    assert second.from_cache is False
    assert second.text == "answer 2"


def test_a_redirect_is_kept_with_where_it_led(server: CacheServer, cache: str | Path) -> None:
    url = server.url("/moved")

    with mitto.Session(cache=cache) as session:
        first = session.get(url)
        second = session.get(url)

    assert server.paths() == ["/moved", "/fresh"]
    assert second.text == first.text == "answer 2"
    assert second.from_cache is True
    assert [(hop.status, hop.url) for hop in second.history] == [(301, url)]
    assert second.url == server.url("/fresh")


def test_a_body_in_the_cache_comes_out_decoded(server: CacheServer, cache: str | Path) -> None:
    url = server.url("/gzip")

    with mitto.Session(cache=cache) as session:
        first = session.get(url)
        second = session.get(url)

    assert first.content == second.content == GZIPPED_TEXT
    assert second.from_cache is True


def test_a_big_body_comes_back_whole(server: CacheServer, cache: str | Path) -> None:
    url = server.url("/big")

    with mitto.Session(cache=cache) as session:
        session.get(url)
        second = session.get(url)

    assert server.paths() == ["/big"]
    assert second.from_cache is True
    assert second.content == BIG_BODY


def test_a_head_is_answered_from_a_get(server: CacheServer, cache: str | Path) -> None:
    url = server.url("/fresh")

    with mitto.Session(cache=cache) as session:
        session.get(url)
        head = session.head(url)

    assert server.paths() == ["/fresh"]
    assert head.from_cache is True
    assert head.content == b""


def test_vary_keeps_apart_what_the_server_says_it_depends_on(server: CacheServer, cache: str | Path) -> None:
    url = server.url("/vary")

    with mitto.Session(cache=cache) as session:
        session.get(url, headers={"X-Choice": "a"})
        same = session.get(url, headers={"X-Choice": "a"})
        other = session.get(url, headers={"X-Choice": "b"})

    assert same.from_cache is True
    assert other.from_cache is False
    assert other.text == "answer 2"


def test_a_callers_no_cache_goes_to_the_server(server: CacheServer, cache: str | Path) -> None:
    url = server.url("/fresh")

    with mitto.Session(cache=cache) as session:
        session.get(url)
        second = session.get(url, headers={"Cache-Control": "no-cache"})

    assert server.paths() == ["/fresh", "/fresh"]
    assert second.from_cache is False


def test_a_callers_condition_gets_the_servers_own_answer(server: CacheServer, cache: str | Path) -> None:
    url = server.url("/etag")

    with mitto.Session(cache=cache) as session:
        session.get(url)
        second = session.get(url, headers={"If-None-Match": ETAG})

    assert second.status == 304
    assert second.from_cache is False


def test_a_post_passes_through_the_cache(server: CacheServer, cache: str | Path) -> None:
    url = server.url("/fresh")

    with mitto.Session(cache=cache) as session:
        session.post(url, data="body")
        second = session.post(url, data="body")

    assert server.paths() == ["/fresh", "/fresh"]
    assert second.from_cache is False


@pytest.mark.parametrize("method", ["POST", "PUT", "PATCH", "DELETE"])
def test_a_request_that_changes_something_drops_what_the_cache_holds(
    server: CacheServer, cache: str | Path, method: str
) -> None:
    url = server.url("/fresh")

    with mitto.Session(cache=cache) as session:
        session.get(url)
        session.request(method, url, data="body")
        after = session.get(url)

    assert server.paths() == ["/fresh"] * 3
    assert after.from_cache is False


def test_a_post_that_fails_leaves_what_the_cache_holds(server: CacheServer, cache: str | Path) -> None:
    fresh = server.url("/fresh")

    with mitto.Session(cache=cache) as session:
        session.get(fresh)
        session.post(server.url("/refused"), data="body")
        after = session.get(fresh)

    assert after.from_cache is True


def test_requests_at_the_same_time_make_one_request_to_the_server(server: CacheServer, cache: str | Path) -> None:
    url = server.url("/slow")

    with mitto.Session(cache=cache) as session, concurrent.futures.ThreadPoolExecutor() as pool:
        responses = list(pool.map(session.get, [url] * 4))

    assert server.paths() == ["/slow"]
    assert {response.text for response in responses} == {"answer 1"}


def test_a_body_that_stops_halfway_is_not_served_as_whole(server: CacheServer, cache: str | Path) -> None:
    url = server.url("/big")

    with _native.Session(http_cache=str(cache)) as session:
        with session.start(url) as request:
            assert isinstance(request.next_event(), _native.ResponseStarted)
            assert request.read() is None
            assert isinstance(request.next_event(), _native.BodyAvailable)
            assert request.read()
        after = session.request(url)

    assert after.body == BIG_BODY
    assert after.from_cache is False


def test_a_stream_says_where_its_body_comes_from(server: CacheServer, cache: str | Path) -> None:
    url = server.url("/big")

    with mitto.Session(cache=cache) as session:
        with session.stream("GET", url) as first:
            first_body = first.read()
        with session.stream("GET", url) as second:
            second_body = b"".join(second.iter_bytes())

    assert server.paths() == ["/big"]
    assert first.from_cache is False
    assert second.from_cache is True
    assert first_body == second_body == BIG_BODY


def test_a_stream_says_so_after_a_revalidation_too(server: CacheServer, cache: str | Path) -> None:
    url = server.url("/etag")

    with mitto.Session(cache=cache) as session:
        session.get(url)
        with session.stream("GET", url) as second:
            body = second.read()

    assert server.received[1].headers["if-none-match"] == ETAG
    assert second.from_cache is True
    assert body == b"answer 1"


def test_a_stream_left_halfway_is_not_served_as_whole(server: CacheServer, cache: str | Path) -> None:
    url = server.url("/big")

    with mitto.Session(cache=cache) as session:
        with session.stream("GET", url) as stream:
            assert next(stream.iter_bytes())
        after = session.get(url)

    assert after.from_cache is False
    assert after.content == BIG_BODY


def test_a_body_of_parts_passes_through_the_cache_and_drops_what_it_holds(
    server: CacheServer, cache: str | Path
) -> None:
    url = server.url("/fresh")

    with mitto.Session(cache=cache) as session:
        session.get(url)
        session.post(url, data=iter([b"first ", b"second"]), headers={"Content-Length": "12"})
        after = session.get(url)

    assert [request.method for request in server.received] == ["GET", "POST", "GET"]
    assert after.from_cache is False


def test_a_memory_cache_goes_with_its_session(server: CacheServer) -> None:
    url = server.url("/fresh")

    with mitto.Session(cache="memory") as session:
        session.get(url)
    with mitto.Session(cache="memory") as session:
        second = session.get(url)

    assert second.from_cache is False
    assert server.paths() == ["/fresh", "/fresh"]


def test_a_memory_cache_makes_no_directory(
    server: CacheServer, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.chdir(tmp_path)

    with mitto.Session(cache="memory") as session:
        session.get(server.url("/fresh"))

    assert list(tmp_path.iterdir()) == []


def test_a_directory_outlives_its_session(server: CacheServer, tmp_path: Path) -> None:
    url = server.url("/fresh")

    with mitto.Session(cache=tmp_path / "cache") as session:
        session.get(url)
    with mitto.Session(cache=tmp_path / "cache") as session:
        second = session.get(url)

    assert server.paths() == ["/fresh"]
    assert second.from_cache is True
    assert second.text == "answer 1"


def test_a_directory_keeps_what_to_revalidate_with(server: CacheServer, tmp_path: Path) -> None:
    url = server.url("/etag")

    with mitto.Session(cache=tmp_path / "cache") as session:
        session.get(url)
    with mitto.Session(cache=tmp_path / "cache") as session:
        second = session.get(url)

    assert server.received[1].headers["if-none-match"] == ETAG
    assert second.from_cache is True
    assert second.text == "answer 1"


def _run_python(code: str) -> subprocess.CompletedProcess[str]:
    """Run code in a process of its own, as another program that uses the library would."""
    return subprocess.run([sys.executable, "-c", textwrap.dedent(code)], capture_output=True, text=True, timeout=120)


@pytest.mark.parametrize("path", ["/fresh", "/big"])
def test_a_directory_outlives_the_process_that_closed_its_session(
    server: CacheServer, tmp_path: Path, path: str
) -> None:
    code = f"""
        import mitto
        with mitto.Session(cache={str(tmp_path / "cache")!r}) as session:
            print(session.get({server.url(path)!r}).from_cache)
        """

    first = _run_python(code)
    second = _run_python(code)

    assert first.stdout.strip() == "False", first.stderr
    assert second.stdout.strip() == "True", second.stderr
    assert server.paths() == [path]


def test_a_process_that_never_closes_its_session_leaves_a_directory_that_opens(
    server: CacheServer, tmp_path: Path
) -> None:
    killed = _run_python(
        f"""
        import os
        import mitto
        session = mitto.Session(cache={str(tmp_path / "cache")!r})
        session.get({server.url("/fresh")!r})
        os._exit(0)
        """
    )
    assert killed.returncode == 0, killed.stderr

    with mitto.Session(cache=tmp_path / "cache") as session:
        assert session.get(server.url("/fresh")).text.startswith("answer ")


def test_a_relative_directory_does_not_follow_the_working_directory(
    server: CacheServer, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    (tmp_path / "elsewhere").mkdir()
    monkeypatch.chdir(tmp_path)

    with mitto.Session(cache="cache") as session:
        monkeypatch.chdir(tmp_path / "elsewhere")
        session.get(server.url("/fresh"))

    assert (tmp_path / "cache" / "Cache_Data" / "index").is_file()
    assert list((tmp_path / "elsewhere").iterdir()) == []


def test_a_directory_named_memory_takes_a_path(
    server: CacheServer, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.chdir(tmp_path)

    with mitto.Session(cache="./memory") as session:
        session.get(server.url("/fresh"))
    with mitto.Session(cache=Path("memory")) as session:
        second = session.get(server.url("/fresh"))

    assert (tmp_path / "memory" / "Cache_Data").is_dir()
    assert second.from_cache is True


def test_a_directory_is_made_with_its_parents(server: CacheServer, tmp_path: Path) -> None:
    with mitto.Session(cache=tmp_path / "a" / "b" / "cache") as session:
        session.get(server.url("/fresh"))

    assert (tmp_path / "a" / "b" / "cache" / "Cache_Data").is_dir()


def test_the_rest_of_a_directory_stays_as_it_was(server: CacheServer, tmp_path: Path) -> None:
    (tmp_path / "index").write_text("not a cache, and not mitto's")
    (tmp_path / "data_0").write_text("not a cache either")

    with mitto.Session(cache=tmp_path) as session:
        session.get(server.url("/fresh"))

    assert (tmp_path / "index").read_text() == "not a cache, and not mitto's"
    assert (tmp_path / "data_0").read_text() == "not a cache either"
    assert (tmp_path / "Cache_Data" / "index").is_file()


def test_a_cache_that_net_cannot_read_starts_over_and_touches_nothing_else(server: CacheServer, tmp_path: Path) -> None:
    url = server.url("/fresh")
    with mitto.Session(cache=tmp_path) as session:
        session.get(url)
    (tmp_path / "Cache_Data" / "index").write_bytes(b"not an index")
    (tmp_path / "mine").write_text("the caller's")

    with mitto.Session(cache=tmp_path) as session:
        after = session.get(url)
        again = session.get(url)

    assert after.from_cache is False
    assert again.from_cache is True
    assert (tmp_path / "mine").read_text() == "the caller's"


def test_a_directory_is_for_one_session_at_a_time(server: CacheServer, tmp_path: Path) -> None:
    url = server.url("/fresh")

    with mitto.Session(cache=tmp_path) as session:
        with pytest.raises(OSError, match="another Session has this cache open") as caught:
            mitto.Session(cache=tmp_path)
        assert caught.value.errno == errno.EBUSY
        assert caught.value.filename == str(tmp_path.resolve())
        session.get(url)
    with mitto.Session(cache=tmp_path) as session:
        assert session.get(url).from_cache is True


def test_another_name_for_a_directory_is_the_same_directory(tmp_path: Path) -> None:
    (tmp_path / "link").symlink_to(tmp_path / "cache", target_is_directory=True)
    (tmp_path / "cache").mkdir()

    with mitto.Session(cache=tmp_path / "cache"), pytest.raises(OSError, match="another Session has this cache open"):
        mitto.Session(cache=tmp_path / "link")


def test_a_session_of_another_process_holds_a_directory_too(tmp_path: Path) -> None:
    code = f"""
        import mitto
        try:
            mitto.Session(cache={str(tmp_path)!r}).close()
        except OSError as error:
            print(error.errno)
        else:
            print("opened")
        """

    with mitto.Session(cache=tmp_path):
        held = _run_python(code)
    freed = _run_python(code)

    assert held.stdout.strip() == str(errno.EBUSY), held.stderr
    assert freed.stdout.strip() == "opened", freed.stderr


def test_a_process_that_ends_lets_go_of_a_directory(tmp_path: Path) -> None:
    ended = _run_python(
        f"""
        import os
        import mitto
        session = mitto.Session(cache={str(tmp_path)!r})
        os._exit(0)
        """
    )
    assert ended.returncode == 0, ended.stderr

    mitto.Session(cache=tmp_path).close()


def test_a_session_that_fails_to_open_lets_go_of_its_directory(tmp_path: Path) -> None:
    (tmp_path / "cookies").write_text("not cookies")

    with pytest.raises(ValueError, match="cookie"):
        mitto.Session(cache=tmp_path / "cache", cookie_file=tmp_path / "cookies")

    mitto.Session(cache=tmp_path / "cache").close()


def test_a_file_in_place_of_a_directory_is_an_error(tmp_path: Path) -> None:
    (tmp_path / "file").write_text("")

    with pytest.raises(OSError, match="ERR_CACHE_OPEN_FAILURE") as caught:
        mitto.Session(cache=tmp_path / "file")
    assert str(tmp_path / "file") in str(caught.value)


@pytest.mark.skipif(os.geteuid() == 0, reason="root can write anywhere")
def test_a_directory_that_cannot_be_written_is_an_error(tmp_path: Path) -> None:
    tmp_path.chmod(0o500)
    try:
        with pytest.raises(OSError, match="ERR_CACHE_OPEN_FAILURE"):
            mitto.Session(cache=tmp_path)
    finally:
        tmp_path.chmod(0o700)


@pytest.mark.parametrize(("cache", "complaint"), [("", "empty string"), ("a\0b", "null")], ids=["empty", "NUL"])
def test_a_string_that_cannot_name_a_directory_is_refused(cache: str, complaint: str) -> None:
    with pytest.raises(ValueError, match=complaint):
        mitto.Session(cache=cache)


def test_something_that_is_not_a_path_is_refused() -> None:
    with pytest.raises(TypeError):
        mitto.Session(cache=1)  # type: ignore[arg-type]  # pyright: ignore[reportArgumentType]


@pytest.mark.parametrize("http_cache", ["", "relative/path", "/a\0b"], ids=["empty", "relative", "NUL"])
def test_the_native_core_takes_only_an_absolute_path(http_cache: str) -> None:
    with pytest.raises(ValueError, match="refused the settings"):
        _native.Session(http_cache=http_cache)


@pytest.mark.parametrize(
    "kind",
    [mitto.RequestKind.SCRIPT, mitto.RequestKind.IMAGE, mitto.RequestKind.FETCH],
    ids=lambda kind: kind.value,
)
def test_two_top_level_sites_do_not_share_a_cached_response(
    server: CacheServer, cache: str | Path, kind: mitto.RequestKind
) -> None:
    url = server.url("/fresh", host=TARGET_HOST)

    with mitto.Session(cache=cache, _settings=ALL_NAMES) as session:
        first = session.get(url, kind=kind, initiator=PAGE)
        other_site = session.get(url, kind=kind, initiator=PAGE_OF_ANOTHER_SITE)
        first_again = session.get(url, kind=kind, initiator=PAGE)
        other_site_again = session.get(url, kind=kind, initiator=PAGE_OF_ANOTHER_SITE)

    assert server.paths() == ["/fresh", "/fresh"]
    assert (first.text, first.from_cache) == ("answer 1", False)
    assert (other_site.text, other_site.from_cache) == ("answer 2", False)
    assert (first_again.text, first_again.from_cache) == ("answer 1", True)
    assert (other_site_again.text, other_site_again.from_cache) == ("answer 2", True)


def test_a_directory_stays_split_for_a_later_session(server: CacheServer, tmp_path: Path) -> None:
    url = server.url("/fresh", host=TARGET_HOST)

    with mitto.Session(cache=tmp_path, _settings=ALL_NAMES) as session:
        session.get(url, kind="script", initiator=PAGE)
    with mitto.Session(cache=tmp_path, _settings=ALL_NAMES) as session:
        other_site = session.get(url, kind="script", initiator=PAGE_OF_ANOTHER_SITE)
        first_site = session.get(url, kind="script", initiator=PAGE)

    assert other_site.from_cache is False
    assert first_site.from_cache is True


def test_a_revalidation_stays_in_its_own_top_level_site(server: CacheServer, cache: str | Path) -> None:
    url = server.url("/etag", host=TARGET_HOST)

    with mitto.Session(cache=cache, _settings=ALL_NAMES) as session:
        session.get(url, kind="script", initiator=PAGE)
        session.get(url, kind="script", initiator=PAGE_OF_ANOTHER_SITE)

    assert "if-none-match" not in server.received[1].headers


def test_pages_on_one_site_share_a_cached_response(server: CacheServer, cache: str | Path) -> None:
    url = server.url("/fresh", host=TARGET_HOST)

    with mitto.Session(cache=cache, _settings=ALL_NAMES) as session:
        session.get(url, kind="script", initiator=PAGE)
        second = session.get(url, kind="script", initiator=PAGE_ON_THE_SAME_SITE)

    assert second.from_cache is True


def test_a_page_on_a_site_shares_with_a_navigation_to_that_site(server: CacheServer, cache: str | Path) -> None:
    url = server.url("/fresh", host=TARGET_HOST)

    with mitto.Session(cache=cache, _settings=ALL_NAMES) as session:
        session.get(url)
        second = session.get(url, kind="script", initiator=f"http://{TARGET_HOST}/page")

    assert second.from_cache is True


def test_a_subresource_with_no_initiator_counts_as_from_its_own_site(server: CacheServer, cache: str | Path) -> None:
    url = server.url("/fresh", host=TARGET_HOST)

    with mitto.Session(cache=cache, _settings=ALL_NAMES) as session:
        session.get(url, kind="script")
        second = session.get(url, kind="script", initiator=f"http://{TARGET_HOST}/page")
        third_party = session.get(url, kind="script", initiator=PAGE)

    assert second.from_cache is True
    assert third_party.from_cache is False


def test_a_cross_site_navigation_keeps_apart_from_one_the_browser_started(
    server: CacheServer, cache: str | Path
) -> None:
    url = server.url("/fresh", host=TARGET_HOST)

    with mitto.Session(cache=cache, _settings=ALL_NAMES) as session:
        session.get(url)
        cross_site = session.get(url, initiator=PAGE)
        from_another_site = session.get(url, initiator=PAGE_OF_ANOTHER_SITE)
        from_its_own_site = session.get(url, initiator=f"http://{TARGET_HOST}/page")

    assert cross_site.from_cache is False
    assert from_another_site.from_cache is True
    assert from_its_own_site.from_cache is True


def test_a_post_only_drops_what_its_own_top_level_site_holds(server: CacheServer, cache: str | Path) -> None:
    url = server.url("/fresh", host=TARGET_HOST)

    with mitto.Session(cache=cache, _settings=ALL_NAMES) as session:
        session.get(url, kind="script", initiator=PAGE)
        session.get(url, kind="fetch", initiator=PAGE_OF_ANOTHER_SITE)
        session.post(url, kind="fetch", initiator=PAGE, data="body")
        own_site = session.get(url, kind="script", initiator=PAGE)
        other_site = session.get(url, kind="fetch", initiator=PAGE_OF_ANOTHER_SITE)

    assert own_site.from_cache is False
    assert other_site.from_cache is True


@pytest.mark.parametrize("protocol", [HTTP1, HTTP2, HTTP3])
def test_a_revalidation_over_each_protocol(cache: str | Path, protocol: str) -> None:
    run = run_of("revalidation", protocol)
    assert run is not None
    path = next(case.path for case in run.cases if case.name == "revalidation")

    with (
        CaptureServer() as capture,
        mitto.Session(cache=cache, _settings=session_settings(capture, run)) as session,
    ):
        url = capture.url(run.route, host=run.host, path=path)
        first = session.get(url, kind="fetch")
        second = session.get(url, kind="fetch")

    assert first.from_cache is False
    assert second.from_cache is True
    assert second.protocol == first.protocol == protocol
    assert second.text == first.text


def test_a_directory_and_a_cookie_file_can_share_a_place(server: CacheServer, tmp_path: Path) -> None:
    cookie_file = tmp_path / "cookies.json"

    with mitto.Session(cache=tmp_path, cookie_file=cookie_file) as session:
        session.get(server.url("/fresh"))
    with mitto.Session(cache=tmp_path, cookie_file=cookie_file) as session:
        second = session.get(server.url("/fresh"))

    assert json.loads(cookie_file.read_text())
    assert second.from_cache is True
