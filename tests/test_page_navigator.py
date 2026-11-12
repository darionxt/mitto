"""Tests that what a Page's navigator, screen and window give is what the same Session sends in its client hints, or what
docs/persona.md works out from its Machine, for several Machines.

A local server asks for every client hint and records what each request carried. Each test goes through both APIs, and
needs a native library with the Page program beside it, as CI's native job installs it on every Host.
"""

import http.server
import math
import threading
from collections.abc import Iterator
from fractions import Fraction
from typing import Any

import pytest

import mitto
from mitto import _native
from mitto._page import Page
from mitto._persona import WINDOWS_PERSONAS
from tests.local_server import QuietServer

pytestmark = pytest.mark.usefixtures("both_apis")

_TIMEOUT_SECONDS = 30.0

# Every client hint that a Persona's Chrome sends once a server asks.
_ALL_HINTS = (
    "Sec-CH-UA-Arch, Sec-CH-UA-Bitness, Sec-CH-UA-Full-Version, Sec-CH-UA-Full-Version-List, Sec-CH-UA-Model, "
    "Sec-CH-UA-Platform-Version, Sec-CH-UA-WoW64, Sec-CH-UA-Form-Factors, Device-Memory, Sec-CH-Device-Memory, DPR, "
    "Sec-CH-DPR, Viewport-Width, Sec-CH-Viewport-Width, Sec-CH-Viewport-Height, RTT, Downlink, ECT, "
    "Sec-CH-Prefers-Color-Scheme, Sec-CH-Prefers-Reduced-Motion, Sec-CH-Prefers-Reduced-Transparency"
)

# Machines that differ in each value a page reads: drawn ones, and one given whole, with a display at 125%, Chrome's
# first window, Windows 10, a touch screen and a slow network.
_MACHINES = [
    pytest.param(lambda: mitto.Machine.draw(seed=1), id="seed-1"),
    pytest.param(lambda: mitto.Machine.draw(seed=2), id="seed-2"),
    pytest.param(lambda: mitto.Machine.draw(seed=217), id="seed-217"),
    pytest.param(
        lambda: mitto.Machine.draw(
            seed=3,
            display_width=1920,
            display_height=1080,
            scale=1.25,
            window_state="normal",
            platform_version="10.0.0",
            memory=32,
            processors=20,
            touch_points=10,
            round_trip_ms=450,
            throughput_kbps=1300,
            color_scheme="dark",
        ),
        id="windows-10-at-125",
    ),
]

# What the page reports of itself, with a fetch() to its own host whose rtt and downlink carry the renderer's noise.
_REPORT = """
    (async () => {
      await fetch("/echo");
      const data = navigator.userAgentData;
      return {
        userAgent: navigator.userAgent,
        languages: navigator.languages,
        deviceMemory: navigator.deviceMemory,
        hardwareConcurrency: navigator.hardwareConcurrency,
        maxTouchPoints: navigator.maxTouchPoints,
        connection: [navigator.connection.effectiveType, navigator.connection.rtt, navigator.connection.downlink],
        devicePixelRatio,
        inner: [innerWidth, innerHeight],
        outer: [outerWidth, outerHeight],
        place: [screenX, screenY, screenLeft, screenTop],
        screen: [screen.width, screen.height],
        available: [screen.availLeft, screen.availTop, screen.availWidth, screen.availHeight],
        brands: data.brands,
        mobile: data.mobile,
        platform: data.platform,
        values: await data.getHighEntropyValues(
          ["architecture", "bitness", "formFactors", "fullVersionList", "model", "platformVersion", "uaFullVersion",
           "wow64"]),
      };
    })()
"""


class _HintsServer:
    """A local server whose page asks for every client hint, which records the header lines of each request."""

    def __init__(self) -> None:
        self.requests: list[tuple[str, dict[str, str]]] = []
        self.lock = threading.Lock()
        server = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self) -> None:
                with server.lock:
                    server.requests.append((self.path, {name.lower(): value for name, value in self.headers.items()}))
                body = b"<!doctype html><p>hints</p>" if self.path == "/" else b"{}"
                self.send_response(200)
                self.send_header("Content-Type", "text/html" if self.path == "/" else "application/json")
                self.send_header("Accept-CH", _ALL_HINTS)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, format: str, *args: Any) -> None:
                pass

        self._server = QuietServer(("127.0.0.1", 0), Handler)
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)

    def url(self, path: str = "/") -> str:
        return f"http://127.0.0.1:{self._server.server_port}{path}"

    def last(self, path: str) -> dict[str, str]:
        """Return the header lines of the last request for `path`, by lower-case name."""
        with self.lock:
            return next(headers for requested, headers in reversed(self.requests) if requested == path)

    def __enter__(self) -> "_HintsServer":
        self._thread.start()
        return self

    def __exit__(self, *exc_info: object) -> None:
        self._server.shutdown()
        self._server.server_close()


@pytest.fixture
def server() -> Iterator[_HintsServer]:
    with _HintsServer() as hints_server:
        yield hints_server


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


def _open_page(session: mitto.Session, url: str) -> Page:
    return session._open_page(url, timeout=_TIMEOUT_SECONDS)  # pyright: ignore[reportPrivateUsage]


def _report(session: mitto.Session, server: _HintsServer) -> dict[str, Any]:
    """Return what a Page of `server` reports, after a navigation that has had the server ask for every hint."""
    session.get(server.url())
    with _open_page(session, server.url()) as page:
        report: dict[str, Any] = page.evaluate(_REPORT, timeout=_TIMEOUT_SECONDS)
    return report


def _brand_list(brands: list[dict[str, str]]) -> str:
    """Write brands as Chrome writes them in sec-ch-ua and sec-ch-ua-full-version-list."""
    return ", ".join(f'"{brand["brand"]}";v="{brand["version"]}"' for brand in brands)


def _number(text: str) -> float:
    return float(text)


@pytest.mark.usefixtures("page_program")
@pytest.mark.parametrize("make_machine", _MACHINES)
def test_a_page_reads_what_its_session_sends(server: _HintsServer, make_machine: Any) -> None:
    machine: mitto.Machine = make_machine()
    with mitto.Session(machine=machine) as session:
        report = _report(session, server)
    navigation = server.last("/")
    fetched = server.last("/echo")

    # What the Persona says, as the hints send it.
    values = report["values"]
    assert report["userAgent"] == navigation["user-agent"]
    assert _brand_list(report["brands"]) == navigation["sec-ch-ua"]
    assert _brand_list(values["brands"]) == navigation["sec-ch-ua"]
    assert _brand_list(values["fullVersionList"]) == navigation["sec-ch-ua-full-version-list"]
    assert ("?1" if report["mobile"] else "?0") == navigation["sec-ch-ua-mobile"]
    assert f'"{report["platform"]}"' == navigation["sec-ch-ua-platform"]
    assert f'"{values["architecture"]}"' == navigation["sec-ch-ua-arch"]
    assert f'"{values["bitness"]}"' == navigation["sec-ch-ua-bitness"]
    assert f'"{values["model"]}"' == navigation["sec-ch-ua-model"]
    assert f'"{values["uaFullVersion"]}"' == navigation["sec-ch-ua-full-version"]
    assert ("?1" if values["wow64"] else "?0") == navigation["sec-ch-ua-wow64"]
    assert (
        ", ".join(f'"{form_factor}"' for form_factor in values["formFactors"]) == navigation["sec-ch-ua-form-factors"]
    )
    assert report["languages"] == ["en-US", "en"]
    assert navigation["accept-language"] == "en-US,en;q=0.9"

    # What the Machine says, as the hints send it.
    assert f'"{values["platformVersion"]}"' == navigation["sec-ch-ua-platform-version"]
    assert report["deviceMemory"] == _number(navigation["device-memory"])
    assert report["devicePixelRatio"] == _number(navigation["dpr"])
    assert report["inner"] == [int(navigation["viewport-width"]), int(navigation["sec-ch-viewport-height"])]
    effective_type, rtt, downlink = report["connection"]
    assert effective_type == navigation["ect"]
    # A subresource of the page's own host carries what its renderer gives that host.
    assert [rtt, downlink] == [int(fetched["rtt"]), _number(fetched["downlink"])]

    # What docs/persona.md works out from the Machine, which no hint sends.
    scale = Fraction(machine.scale)
    assert report["screen"] == [
        math.ceil(machine.display_width / scale),
        math.ceil(machine.display_height / scale),
    ]
    assert report["available"] == [
        machine.work_area_x,
        machine.work_area_y,
        machine.work_area_width,
        machine.work_area_height,
    ]
    assert report["outer"] == [machine.window_width, machine.window_height]
    assert report["place"] == [machine.window_x, machine.window_y, machine.window_x, machine.window_y]
    assert report["hardwareConcurrency"] == machine.processors
    assert report["maxTouchPoints"] == machine.touch_points


@pytest.mark.usefixtures("page_program")
def test_a_pages_languages_follow_the_sessions_accept_language(server: _HintsServer) -> None:
    with mitto.Session(accept_language="de-DE,de;q=0.9,en;q=0.8") as session:
        report = _report(session, server)

    assert server.last("/")["accept-language"] == "de-DE,de;q=0.9,en;q=0.8"
    assert report["languages"] == ["de-DE", "de", "en"]


@pytest.mark.usefixtures("page_program")
def test_each_persona_gives_a_page_what_it_sends(server: _HintsServer) -> None:
    for persona in WINDOWS_PERSONAS.values():
        with mitto.Session(persona=persona) as session:
            report = _report(session, server)
        navigation = server.last("/")
        assert report["userAgent"] == navigation["user-agent"], persona
        assert _brand_list(report["brands"]) == navigation["sec-ch-ua"], persona
        assert _brand_list(report["values"]["fullVersionList"]) == navigation["sec-ch-ua-full-version-list"], persona
