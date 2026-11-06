"""The capture kit against a real Chrome. It only runs where MITTO_CHROME names a Chrome executable.

Chrome runs headless, so this needs no display and takes no one's screen. It starts from a new user data directory
every time, and it reaches nothing but the capture server's pages.
"""

import functools
import json
import os
import subprocess
import tempfile
from pathlib import Path

import pytest

from tools.capture import HTTP1, HTTP2, HTTP3, RUNS, CaptureServer, Run, normal_form
from tools.capture.devtools import PageClicker
from tools.capture.recording import Json

_CHROME_ENV_VAR = "MITTO_CHROME"
_PAGE_TIMEOUT_SECONDS = 60.0
_EXIT_TIMEOUT_SECONDS = 10.0
_RUNS_BY_NAME = {run.name: run for run in RUNS}
_TLS_RUNS = ("cases/http1", "cases/http2", "cases/http3")
_CLIENT_HINTS_RUNS = ("cases/client_hints_http1", "cases/client_hints_http2", "cases/client_hints_http3")
_PROXY_RUNS = ("cases/proxy_http1", "cases/proxy_http2")
_PROTOCOLS = {
    "http1": "http/1.1",
    "http2": "h2",
    "http3": "h3",
    "cases/http1": "http/1.1",
    "cases/http2": "h2",
    "cases/http3": "h3",
    "cases/plain_http": "http/1.1",
    "cases/proxy_http1": "http/1.1",
    "cases/proxy_http2": "h2",
    "cases/client_hints_http1": "http/1.1",
    "cases/client_hints_http2": "h2",
    "cases/client_hints_http3": "h3",
    "cases/client_hints_plain_http": "http/1.1",
}
_NAVIGATE = ("navigate", "document")
_CORS = ("cors", "empty")
_SITE_MODE_AND_DESTINATION = {
    "navigation_link_same_origin": ("same-origin", *_NAVIGATE),
    "navigation_link_same_site": ("same-site", *_NAVIGATE),
    "navigation_link_cross_site": ("cross-site", *_NAVIGATE),
    "navigation_form_urlencoded": ("same-origin", *_NAVIGATE),
    "navigation_form_multipart": ("same-origin", *_NAVIGATE),
    "fetch_get_same_site": ("same-site", *_CORS),
    "fetch_get_cross_site": ("cross-site", *_CORS),
    "fetch_post_text_same_origin": ("same-origin", *_CORS),
    "fetch_post_text_cross_site": ("cross-site", *_CORS),
    "fetch_post_json_same_origin": ("same-origin", *_CORS),
    "script_same_site": ("same-site", "no-cors", "script"),
    "script_cross_site": ("cross-site", "no-cors", "script"),
    "image_same_site": ("same-site", "no-cors", "image"),
    "image_cross_site": ("cross-site", "no-cors", "image"),
    "cookie": ("same-origin", *_CORS),
    "revalidation": ("same-origin", *_CORS),
    "redirect_navigation_1": ("same-origin", *_NAVIGATE),
    "redirect_navigation_2": ("same-site", *_NAVIGATE),
    "redirect_navigation_3": ("cross-site", *_NAVIGATE),
    # Once a chain has been cross-site, it stays that, also on its way back.
    "redirect_navigation_4": ("cross-site", *_NAVIGATE),
    "redirect_fetch_1": ("same-origin", *_CORS),
    "redirect_fetch_2": ("same-site", *_CORS),
    "redirect_fetch_3": ("cross-site", *_CORS),
    "redirect_fetch_4": ("cross-site", *_CORS),
}


@pytest.fixture(scope="module")
def chrome() -> str:
    path = os.environ.get(_CHROME_ENV_VAR)
    if not path:
        pytest.skip(f"set {_CHROME_ENV_VAR} to a Chrome executable to run this test")
    return path


def record(chrome: str, run: Run) -> dict[str, Json]:
    """Take a Chrome with a fresh user data directory through a run, and return the normal form of what it sent."""
    with (
        CaptureServer() as server,
        tempfile.TemporaryDirectory(prefix="mitto-capture-", ignore_cleanup_errors=True) as user_data,
    ):
        process = subprocess.Popen(
            [
                chrome,
                "--headless=new",
                f"--user-data-dir={user_data}",
                "--remote-debugging-port=0",
                *server.chrome_flags(run),
                "--no-first-run",
                "--no-default-browser-check",
                server.start_url(run),
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            with PageClicker(Path(user_data)):
                assert server.wait_for_page(run.route, _PAGE_TIMEOUT_SECONDS, done_path=run.done_path), (
                    f"{run.name} did not finish"
                )
        finally:
            process.terminate()
            try:
                process.wait(_EXIT_TIMEOUT_SECONDS)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        return normal_form(server.recording(run.route, host=run.host), run.cases)


@functools.cache
def recorded(chrome: str, name: str) -> dict[str, Json]:
    """Return one recording of a run, so that the tests that only read it share it."""
    return record(chrome, _RUNS_BY_NAME[name])


def request_of(form: dict[str, Json], case: str) -> dict[str, Json]:
    requests = form["requests"]
    assert isinstance(requests, dict)
    request = requests[case]
    assert isinstance(request, dict)
    return request


def headers_of(form: dict[str, Json], case: str) -> dict[str, str]:
    """Return a request's headers with their names in lower case, the same over every protocol."""
    lines = request_of(form, case)["headers"]
    assert isinstance(lines, list)
    pairs = (str(line).lstrip(":").partition(": ") for line in lines)
    return {name.lower(): value for name, _, value in pairs}


@pytest.mark.parametrize("name", [run.name for run in RUNS])
def test_two_recordings_have_the_same_normal_form(chrome: str, name: str) -> None:
    run = _RUNS_BY_NAME[name]

    first = recorded(chrome, name)
    second = record(chrome, run)

    assert json.dumps(first, indent=2) == json.dumps(second, indent=2)
    assert first["protocol"] == _PROTOCOLS[name]
    requests = first["requests"]
    assert isinstance(requests, dict)
    assert list(requests) == [case.name for case in run.cases]


@pytest.mark.parametrize("name", _TLS_RUNS)
@pytest.mark.parametrize(("case", "expected"), _SITE_MODE_AND_DESTINATION.items())
def test_each_case_is_what_it_says_it_is(chrome: str, name: str, case: str, expected: tuple[str, str, str]) -> None:
    headers = headers_of(recorded(chrome, name), case)

    assert (headers["sec-fetch-site"], headers["sec-fetch-mode"], headers["sec-fetch-dest"]) == expected
    # The clicks have to count as the user's, as a person's do.
    assert ("sec-fetch-user" in headers) == (expected[1] == "navigate")


@pytest.mark.parametrize("name", _TLS_RUNS)
def test_real_chrome_sends_what_the_cases_carry(chrome: str, name: str) -> None:
    form = recorded(chrome, name)

    assert request_of(form, "navigation_form_urlencoded")["body"] == "name=mitto&a+b%26c=d+e%26f%2Bg"
    assert headers_of(form, "navigation_form_urlencoded")["content-type"] == "application/x-www-form-urlencoded"
    assert headers_of(form, "navigation_form_multipart")["content-type"] == (
        "multipart/form-data; boundary=----WebKitFormBoundary<random>"
    )
    multipart = request_of(form, "navigation_form_multipart")["body"]
    assert isinstance(multipart, str)
    assert multipart.startswith("------WebKitFormBoundary<random>\r\n")
    assert 'name="a %22b%22"' in multipart
    assert 'filename="file %22name%22.txt"' in multipart
    assert request_of(form, "fetch_post_text_same_origin")["body"] == "text body"
    assert request_of(form, "fetch_post_text_cross_site")["body"] == "text body"
    assert headers_of(form, "fetch_post_text_cross_site")["origin"] == "<origin>"
    assert headers_of(form, "fetch_post_text_cross_site")["authority" if name != "cases/http1" else "host"] == (
        "<cross-site-authority>"
    )
    assert request_of(form, "fetch_post_json_same_origin")["body"] == '{"json":"body"}'
    assert headers_of(form, "cookie")["cookie"] == "mitto=1"
    assert headers_of(form, "revalidation")["if-none-match"] == '"mitto"'
    assert headers_of(form, "navigation_link_cross_site")["referer"] == "<same-site-origin>/"
    assert "origin" not in headers_of(form, "redirect_fetch_1")
    assert headers_of(form, "redirect_fetch_2")["origin"] == "<origin>"
    assert headers_of(form, "redirect_fetch_3")["origin"] == "null"


@pytest.mark.parametrize(("name", "protocol"), [("cases/http1", HTTP1), ("cases/http2", HTTP2), ("cases/http3", HTTP3)])
def test_chrome_sends_a_stream_body_only_above_http1(chrome: str, name: str, protocol: str) -> None:
    request = request_of(recorded(chrome, name), "fetch_post_stream")

    if protocol == HTTP1:
        assert request == {"refused": True}
    else:
        assert request["body"] == "stream body"


def test_plain_http_to_a_host_that_is_not_local_is_not_potentially_trustworthy(chrome: str) -> None:
    form = recorded(chrome, "cases/plain_http")

    assert "tls" not in form
    for case in ("plain_http_navigation", "plain_http_fetch", "plain_http_script", "plain_http_image"):
        headers = headers_of(form, case)
        assert headers["accept-encoding"] == "gzip, deflate"
        assert not [name for name in headers if name.startswith(("sec-ch-", "sec-fetch-"))]


@pytest.mark.parametrize("name", _PROXY_RUNS)
def test_https_through_a_proxy_starts_with_connect(chrome: str, name: str) -> None:
    form = recorded(chrome, name)

    connect = request_of(form, "proxy_connect")
    assert connect["request_line"] == "CONNECT <authority> HTTP/1.1"
    headers = headers_of(form, "proxy_connect")
    assert list(headers) == ["host", "proxy-connection", "user-agent"]
    assert headers["host"] == "<authority>"
    assert headers_of(form, "proxy_navigation")["sec-fetch-site"] == "none"
    assert "tls" in form


_SENT_BY_DEFAULT = {"sec-ch-ua", "sec-ch-ua-mobile", "sec-ch-ua-platform"}
_ASKED_FOR = "sec-ch-ua-arch"


def _hints_of(headers: dict[str, str]) -> set[str]:
    return {name for name in headers if name.startswith("sec-ch-")}


@pytest.mark.parametrize("name", _CLIENT_HINTS_RUNS)
def test_chrome_sends_what_a_server_asked_for_only_to_that_origin(chrome: str, name: str) -> None:
    form = recorded(chrome, name)

    assert _hints_of(headers_of(form, "client_hints_first")) == _SENT_BY_DEFAULT
    for case in (
        "client_hints_script",
        "client_hints_image",
        "client_hints_fetch",
        "client_hints_fetch_post",
        "client_hints_navigation",
        "client_hints_navigation_back",
        "client_hints_form",
        "client_hints_redirect_fetch_1",
        "client_hints_redirect_navigation_1",
        "client_hints_redirect_navigation_3",
        "client_hints_redirect_navigation_5",
    ):
        assert _ASKED_FOR in headers_of(form, case), case
    for case in (
        "client_hints_script_same_site",
        "client_hints_script_cross_site",
        "client_hints_image_same_site",
        "client_hints_image_cross_site",
        "client_hints_fetch_same_site",
        "client_hints_fetch_cross_site",
        "client_hints_redirect_fetch_2",
        "client_hints_redirect_fetch_3",
        "client_hints_redirect_fetch_4",
        "client_hints_navigation_same_site",
        "client_hints_navigation_cross_site",
        "client_hints_redirect_navigation_2",
        "client_hints_redirect_navigation_4",
        "client_hints_none_fetch",
        "client_hints_none_navigation",
    ):
        assert _hints_of(headers_of(form, case)) == _SENT_BY_DEFAULT, case


@pytest.mark.parametrize("name", _CLIENT_HINTS_RUNS)
def test_chrome_sends_no_width_for_an_image_with_no_width(chrome: str, name: str) -> None:
    headers = headers_of(recorded(chrome, name), "client_hints_image")

    assert "width" not in headers
    assert "sec-ch-width" not in headers


@pytest.mark.parametrize("name", _CLIENT_HINTS_RUNS)
def test_chrome_starts_a_navigation_over_for_a_critical_hint(chrome: str, name: str) -> None:
    assert _ASKED_FOR in headers_of(recorded(chrome, name), "client_hints_critical")


def test_plain_http_cannot_ask_for_hints(chrome: str) -> None:
    form = recorded(chrome, "cases/client_hints_plain_http")

    for case in ("client_hints_plain_http_fetch", "client_hints_plain_http_navigation"):
        assert not _hints_of(headers_of(form, case))
