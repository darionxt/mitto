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

from tests.capture.test_script_normal_form import getters_of
from tools.capture import (
    DOCUMENT_RUNS,
    HTTP1,
    HTTP2,
    HTTP3,
    RECORDING_RUNS,
    RESUMPTION_RUNS,
    CaptureServer,
    Recording,
    Run,
    normal_form,
)
from tools.capture.devtools import PageClicker
from tools.capture.http1 import Http1Request
from tools.capture.probe import RecordValue, dumps_record, read_record
from tools.capture.recording import Json
from tools.capture.script_normal_form import machine_values, script_normal_form
from tools.capture.script_page import DEDICATED_WORKER, SCRIPT_RUNS, ScriptRun

_CHROME_ENV_VAR = "MITTO_CHROME"
_PAGE_TIMEOUT_SECONDS = 60.0
_EXIT_TIMEOUT_SECONDS = 10.0
_RUNS_BY_NAME = {run.name: run for run in RECORDING_RUNS}
_TLS_RUNS = ("cases/http1", "cases/http2", "cases/http3")
_CLIENT_HINTS_RUNS = ("cases/client_hints_http1", "cases/client_hints_http2", "cases/client_hints_http3")
_PROXY_RUNS = ("cases/proxy_http1", "cases/proxy_http2")
_DOCUMENT_RUNS = tuple(run.name for run in DOCUMENT_RUNS)
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
    "documents/http1": "http/1.1",
    "documents/http2": "h2",
    "documents/http3": "h3",
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
_NO_CORS_SCRIPT = ("no-cors", "script")
_NO_CORS_STYLE = ("no-cors", "style")
_NO_CORS_IMAGE = ("no-cors", "image")
_CORS_FONT = ("cors", "font")
_MODULE_SCRIPT = ("cors", "script")
_WORKER = ("same-origin", "worker")
_BEACON = ("no-cors", "empty")
_FRAME = ("navigate", "iframe")
_DOCUMENT_SITE_MODE_AND_DESTINATION = {
    "style": ("same-origin", *_NO_CORS_STYLE),
    "style_import": ("same-origin", *_NO_CORS_STYLE),
    "style_cross_site": ("cross-site", *_NO_CORS_STYLE),
    "font": ("same-origin", *_CORS_FONT),
    "font_cross_site": ("cross-site", *_CORS_FONT),
    "module": ("same-origin", *_MODULE_SCRIPT),
    "module_static_import": ("same-origin", *_MODULE_SCRIPT),
    "module_dynamic_import": ("same-origin", *_MODULE_SCRIPT),
    "module_cross_site": ("cross-site", *_MODULE_SCRIPT),
    "xhr_get": ("same-origin", *_CORS),
    "xhr_get_cross_site": ("cross-site", *_CORS),
    "xhr_post_text": ("same-origin", *_CORS),
    "xhr_get_sync": ("same-origin", *_CORS),
    "xhr_post_text_sync": ("same-origin", *_CORS),
    "worker": ("same-origin", *_WORKER),
    "worker_import_scripts": ("same-origin", *_NO_CORS_SCRIPT),
    "worker_fetch": ("same-origin", *_CORS),
    "worker_module": ("same-origin", *_WORKER),
    "worker_blob_fetch": ("same-origin", *_CORS),
    "beacon_text": ("same-origin", *_BEACON),
    "beacon_blob": ("same-origin", *_BEACON),
    "beacon_form_data": ("same-origin", *_BEACON),
    "beacon_text_cross_site": ("cross-site", *_BEACON),
    "fetch_later": ("same-origin", *_CORS),
    "preflight": ("cross-site", *_CORS),
    "fetch_post_json_cross_site": ("cross-site", *_CORS),
    "fetch_credentials_omit": ("same-site", *_CORS),
    "fetch_credentials_same_origin": ("same-site", *_CORS),
    "fetch_credentials_include": ("same-site", *_CORS),
    "fetch_no_cors": ("cross-site", "no-cors", "empty"),
    "navigation_location": ("same-origin", *_NAVIGATE),
    "navigation_click": ("same-origin", *_NAVIGATE),
    "navigation_meta_refresh": ("same-origin", *_NAVIGATE),
    "redirect_location_1": ("same-origin", *_NAVIGATE),
    "redirect_location_2": ("cross-site", *_NAVIGATE),
    "frame_same_origin": ("same-origin", *_FRAME),
    "frame_same_site": ("same-site", *_FRAME),
    "frame_cross_site": ("cross-site", *_FRAME),
    # From the frame of the other site, to that site.
    "frame_script": ("same-origin", *_NO_CORS_SCRIPT),
    "frame_image": ("same-origin", *_NO_CORS_IMAGE),
    "frame_fetch": ("same-origin", *_CORS),
    # The page's origin again, which the frame of the other site asks for.
    "frame_nested": ("cross-site", *_FRAME),
    "frame_nested_fetch": ("same-origin", *_CORS),
    "document_style": ("same-origin", *_NO_CORS_STYLE),
    "document_preload_style": ("same-origin", *_NO_CORS_STYLE),
    "document_preload_script": ("same-origin", *_NO_CORS_SCRIPT),
    "document_preload_image": ("same-origin", *_NO_CORS_IMAGE),
    "document_preload_font": ("same-origin", *_CORS_FONT),
    "document_script": ("same-origin", *_NO_CORS_SCRIPT),
    "document_image_small": ("same-origin", *_NO_CORS_IMAGE),
    "document_image": ("same-origin", *_NO_CORS_IMAGE),
    "document_style_body": ("same-origin", *_NO_CORS_STYLE),
    "document_script_deferred": ("same-origin", *_NO_CORS_SCRIPT),
    "navigation_form_get": ("same-origin", *_NAVIGATE),
    "navigation_form_text": ("same-origin", *_NAVIGATE),
    "navigation_form_cross_site": ("cross-site", *_NAVIGATE),
    # The page it goes back to was reached from a page of the other site, and the traversal keeps that Initiator.
    "navigation_history_back": ("cross-site", *_NAVIGATE),
}
_CLICKED = {"navigation_form_get", "navigation_form_text", "navigation_form_cross_site"}
"""The navigations of the document runs that a user starts. The page starts the others, with no user activation."""


@pytest.fixture(scope="module")
def chrome() -> str:
    path = os.environ.get(_CHROME_ENV_VAR)
    if not path:
        pytest.skip(f"set {_CHROME_ENV_VAR} to a Chrome executable to run this test")
    return path


def go_through(chrome: str, run: Run, *, tickets: bool = False) -> Recording:
    """Take a Chrome with a fresh user data directory through a run, and return what it sent."""
    with (
        CaptureServer(tickets=tickets) as server,
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
        return server.recording(run.route, host=run.host)


def record(chrome: str, run: Run) -> dict[str, Json]:
    """Take a Chrome with a fresh user data directory through a run, and return the normal form of what it sent."""
    return normal_form(go_through(chrome, run), run.cases, run.ordered)


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


@pytest.mark.parametrize("name", [run.name for run in RECORDING_RUNS])
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


def method_and_target(form: dict[str, Json], case: str) -> tuple[str, str]:
    """Return a request's method and target, the same over every protocol."""
    request_line = request_of(form, case).get("request_line")
    if isinstance(request_line, str):
        method, target, _ = request_line.split(" ")
        return method, target
    headers = headers_of(form, case)
    return headers["method"], headers["path"]


@pytest.mark.parametrize("name", _DOCUMENT_RUNS)
@pytest.mark.parametrize(("case", "expected"), _DOCUMENT_SITE_MODE_AND_DESTINATION.items())
def test_each_document_case_is_what_it_says_it_is(
    chrome: str, name: str, case: str, expected: tuple[str, str, str]
) -> None:
    headers = headers_of(recorded(chrome, name), case)

    assert (headers["sec-fetch-site"], headers["sec-fetch-mode"], headers["sec-fetch-dest"]) == expected
    # Only a person's click counts as the user's, and what a page starts by itself does not.
    assert ("sec-fetch-user" in headers) == (case in _CLICKED)


@pytest.mark.parametrize("name", _DOCUMENT_RUNS)
def test_real_chrome_sends_what_the_document_cases_carry(chrome: str, name: str) -> None:
    form = recorded(chrome, name)

    for case in ("xhr_post_text", "xhr_post_text_sync", "beacon_text", "beacon_text_cross_site"):
        assert method_and_target(form, case)[0] == "POST", case
        assert request_of(form, case)["body"] == "text body", case
        assert headers_of(form, case)["content-type"] == "text/plain;charset=UTF-8", case
    assert request_of(form, "beacon_blob")["body"] == "blob body"
    assert "content-type" not in headers_of(form, "beacon_blob")
    assert headers_of(form, "beacon_form_data")["content-type"] == (
        "multipart/form-data; boundary=----WebKitFormBoundary<random>"
    )
    assert 'name="name"\r\n\r\nmitto\r\n' in str(request_of(form, "beacon_form_data")["body"])
    assert method_and_target(form, "preflight")[0] == "OPTIONS"
    assert "body" not in request_of(form, "preflight")
    assert headers_of(form, "preflight")["access-control-request-method"] == "POST"
    assert headers_of(form, "preflight")["access-control-request-headers"] == "content-type"
    assert method_and_target(form, "fetch_post_json_cross_site")[0] == "POST"
    assert request_of(form, "fetch_post_json_cross_site")["body"] == '{"json":"body"}'
    for case in ("font_cross_site", "module_cross_site", "xhr_get_cross_site", "fetch_post_json_cross_site"):
        assert headers_of(form, case)["origin"] == "<origin>", case
    assert "origin" not in headers_of(form, "fetch_no_cors")
    assert headers_of(form, "fetch_credentials_include")["cookie"] == "mitto=1"
    assert "cookie" not in headers_of(form, "fetch_credentials_omit")
    assert "cookie" not in headers_of(form, "fetch_credentials_same_origin")
    assert method_and_target(form, "navigation_form_get") == (
        "GET",
        "/case/navigation-form-get?name=mitto&a+b%26c=d+e%26f%2Bg",
    )
    assert request_of(form, "navigation_form_text")["body"] == "name=mitto\r\na b&c=d e&f+g\r\n"
    assert headers_of(form, "navigation_form_text")["content-type"] == "text/plain"
    assert request_of(form, "navigation_form_cross_site")["body"] == "name=mitto&a+b%26c=d+e%26f%2Bg"
    assert headers_of(form, "navigation_form_cross_site")["origin"] == "<origin>"
    assert headers_of(form, "navigation_history_back")["cookie"] == "mitto=1"


def header_names(form: dict[str, Json], case: str) -> list[str]:
    """Return the names of a request's headers in the order sent, in lower case and without pseudo-headers."""
    lines = request_of(form, case)["headers"]
    assert isinstance(lines, list)
    return [str(line).partition(": ")[0].lower() for line in lines if not str(line).startswith(":")]


@pytest.mark.parametrize("name", _DOCUMENT_RUNS)
def test_a_worker_s_requests_carry_no_client_hints_and_the_browser_asks_for_its_script(chrome: str, name: str) -> None:
    form = recorded(chrome, name)

    # A window's request has those sent by default, and a worker's none.
    assert "sec-ch-ua" in header_names(form, "xhr_get")
    for case in ("worker", "worker_module", "worker_import_scripts", "worker_fetch", "worker_blob_fetch"):
        assert not [header for header in header_names(form, case) if header.startswith("sec-ch-")], case
    # The browser makes the request for a worker's script, and puts User-Agent where its own requests have it.
    for case in ("worker", "worker_module"):
        names = header_names(form, case)
        assert names.index("referer") < names.index("user-agent"), case
    names = header_names(form, "worker_fetch")
    assert names.index("user-agent") < names.index("referer")
    # A blob: URL is no referrer.
    assert "referer" not in header_names(form, "worker_blob_fetch")
    assert headers_of(form, "worker_fetch")["referer"] == "<origin>/case/worker"


@pytest.mark.parametrize("name", _DOCUMENT_RUNS)
def test_a_frame_of_the_page_s_site_under_another_site_gets_only_third_party_cookies(chrome: str, name: str) -> None:
    form = recorded(chrome, name)

    for case in ("frame_nested", "frame_nested_fetch"):
        headers = headers_of(form, case)
        assert headers["cookie"] == "none=1", case
        assert headers["sec-fetch-storage-access"] == "active", case
    assert headers_of(form, "frame_nested")["referer"] == "<cross-site-origin>/"


@pytest.mark.parametrize("name", _DOCUMENT_RUNS)
def test_the_order_of_what_the_document_asks_for_is_chrome_s_over_one_connection(chrome: str, name: str) -> None:
    form = recorded(chrome, name)

    if form["protocol"] == "http/1.1":
        assert "order" not in form
    else:
        order = form["order"]
        assert isinstance(order, list)
        assert sorted(map(str, order)) == sorted(case.name for case in _RUNS_BY_NAME[name].ordered)


@pytest.mark.parametrize("name", _DOCUMENT_RUNS)
def test_chrome_asks_for_an_icon_as_an_image_of_the_page_if_it_asks(chrome: str, name: str) -> None:
    form = recorded(chrome, name)

    if request_of(form, "favicon") != {"refused": True}:
        assert method_and_target(form, "favicon") == ("GET", "/favicon.ico")
        headers = headers_of(form, "favicon")
        assert (headers["sec-fetch-site"], headers["sec-fetch-mode"], headers["sec-fetch-dest"]) == (
            "same-origin",
            *_NO_CORS_IMAGE,
        )


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


@pytest.mark.parametrize("run", RESUMPTION_RUNS, ids=lambda run: run.name)
def test_real_chrome_adds_only_what_a_ticket_takes_to_a_client_hello(chrome: str, run: Run) -> None:
    recording = go_through(chrome, run, tickets=True)
    fresh = normal_form(recording, run.fresh_cases)
    resumed = normal_form(recording, run.cases)

    added = {"pre_shared_key", "early_data"} if run.route == HTTP3 else {"pre_shared_key"}
    fresh_tls = fresh["tls"]
    resumed_tls = resumed["tls"]
    assert isinstance(fresh_tls, dict)
    assert isinstance(resumed_tls, dict)
    fresh_extensions = fresh_tls["extensions"]
    resumed_extensions = resumed_tls["extensions"]
    assert isinstance(fresh_extensions, list)
    assert isinstance(resumed_extensions, list)
    assert [
        extension
        for extension in resumed_extensions
        if not (isinstance(extension, dict) and extension["type"] in added)
    ] == fresh_extensions
    assert len(resumed_extensions) == len(fresh_extensions) + len(added)
    for part in ("protocol", "quic", "http2", "http3"):
        assert resumed.get(part) == fresh.get(part)


def test_real_chrome_sends_only_what_is_safe_to_repeat_as_early_data(chrome: str) -> None:
    run = next(run for run in RESUMPTION_RUNS if run.route == HTTP3)
    resumed = normal_form(go_through(chrome, run, tickets=True), run.cases)

    assert request_of(resumed, "resumption_fetch")["early_data"] == ["request", "control", "qpack_encoder"]
    assert request_of(resumed, "resumption_navigation")["early_data"] == ["request", "control", "qpack_encoder"]
    assert request_of(resumed, "resumption_post")["early_data"] == ["control"]


def record_script_fingerprint(chrome: str, run: ScriptRun) -> dict[str, RecordValue]:
    """Take a Chrome with a fresh user data directory through a run of a Script Fingerprint, with no debugging port, and
    return the Normal Form of what its page posted."""
    with (
        CaptureServer() as server,
        tempfile.TemporaryDirectory(prefix="mitto-capture-", ignore_cleanup_errors=True) as user_data,
    ):
        process = subprocess.Popen(
            [
                chrome,
                "--headless=new",
                f"--user-data-dir={user_data}",
                *server.chrome_flags(run),
                "--no-first-run",
                "--no-default-browser-check",
                server.start_url(run),
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
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
        recording = server.recording(run.route, host=run.host)
    (body,) = [
        request.body
        for connection in recording.connections
        for request in connection.requests
        if isinstance(request, Http1Request) and request.target == run.done_path
    ]
    return script_normal_form(read_record(body.decode()), server.start_url(run), run.machine_places)


@pytest.mark.parametrize("run", SCRIPT_RUNS, ids=lambda run: run.name)
def test_two_records_of_a_script_fingerprint_have_the_same_normal_form(chrome: str, run: ScriptRun) -> None:
    first = record_script_fingerprint(chrome, run)
    second = record_script_fingerprint(chrome, run)

    assert dumps_record(first) == dumps_record(second)
    assert [value for _, value in machine_values(first, run.machine_places)] == ["<machine>"] * len(run.machine_places)
    # A start with no debugging port, and with none of those that make a browser under automation. A worker's navigator
    # has no webdriver.
    if run != DEDICATED_WORKER:
        assert getters_of(first, "navigator")["webdriver"] is False
