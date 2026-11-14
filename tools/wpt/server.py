"""`wpt serve` from the copy of web-platform-tests, on Chromium's ports, with mitto's report of each test's results."""

import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from types import TracebackType
from typing import Any, Self
from urllib.parse import parse_qs, urlsplit

from mitto._native import PrivateSettings
from tools.wpt.files import WPT_ROOT
from tools.wpt.reporter.collect import RESULTS_ENV_VAR, result_key

__all__ = ["BROWSER_HOST", "HOST_RESOLVER_RULES", "PORTS", "SPKI", "ServerError", "WptServer"]

BROWSER_HOST = "web-platform.test"

PORTS: dict[str, list[int]] = {
    "http": [8001, 8081],
    "http-local": [8082],
    "http-public": [8093],
    "https": [8444, 8445],
    "https-local": [8446],
    "https-public": [8447],
    "h2": [9000],
    "ws": [9001],
    "wss": [9444],
}
"""Chromium's ports for web-platform-tests, from its config.tmpl.json, so that messages that name an origin, such as
"http://web-platform.test:8001", are those of Chromium's runs."""

SPKI = "RtijU6YWXgOa1vlrp14TaeWmmoWCao6Tmbqf6ybUV0E="
"""The key of the copy's certificate, tools/certs/web-platform.test.pem, as wptrunner gives it to Chrome."""

HOST_RESOLVER_RULES = "MAP nonexistent.*.test ^NOTFOUND, MAP *.test 127.0.0.1, MAP *.test. 127.0.0.1"
"""How wptrunner sends the test names to loopback."""

_REPORTER = Path(__file__).parent / "reporter"
_START_SECONDS = 60.0
_SERVE = """
import sys
root = sys.argv[1]
sys.path.insert(0, root + "/tools")
import localpaths
from serve import serve
sys.exit(serve.run(config_path=sys.argv[2]))
"""

_STOP_SECONDS = 10.0


class ServerError(Exception):
    """The server did not start."""


class WptServer:
    """`wpt serve` in a process of its own, with what tests post kept in a directory. Use it in a with statement.

    Attributes:
        results: Where the posted results go, one file per test, with the server's log.
    """

    def __init__(self, results: Path | None = None, root: Path = WPT_ROOT, log_level: str = "warning") -> None:
        """Make the server.

        Args:
            results: Where to keep what the tests post, or None for a temporary directory that goes with the server.
            root: The root of web-platform-tests to serve.
            log_level: What the server logs: "warning" logs what a handler raised, such as a file it could not open,
                and "debug" also logs each response, with its status.
        """
        self._log_level = log_level
        self._root = root.resolve()
        self._temporary: tempfile.TemporaryDirectory[str] | None = None
        if results is None:
            # On Windows, a process of the server can hold its log a moment after it stopped.
            self._temporary = tempfile.TemporaryDirectory(prefix="mitto-wpt-", ignore_cleanup_errors=True)
            results = Path(self._temporary.name)
        self.results = results
        self._process: subprocess.Popen[bytes] | None = None

    def __enter__(self) -> Self:
        self.start()
        return self

    def __exit__(
        self, exc_type: type[BaseException] | None, exc: BaseException | None, traceback: TracebackType | None
    ) -> None:
        self.stop()

    @property
    def settings(self) -> PrivateSettings:
        """What a Session needs to reach the server: its names on loopback, and its certificate trusted."""
        return PrivateSettings(ignore_certificate_errors_spki_list=[SPKI], host_resolver_rules=HOST_RESOLVER_RULES)

    def start(self) -> None:
        """Start the server, and wait until every port answers.

        Raises:
            ServerError: The server exited, or a port did not answer within a minute, such as one that another program
                holds.
        """
        self.results.mkdir(parents=True, exist_ok=True)
        config = self.results / "config.json"
        config.write_text(json.dumps(self._config(), indent=2), encoding="utf-8")
        log = (self.results / "server.log").open("wb")
        environment = {**os.environ, RESULTS_ENV_VAR: str(self.results)}
        self._process = subprocess.Popen(
            [sys.executable, "-I", "-c", _SERVE, str(self._root), str(config)],
            cwd=self._root,
            env=environment,
            stdout=log,
            stderr=subprocess.STDOUT,
            # A group of its own, so that stop() can end the processes that the server starts too.
            start_new_session=sys.platform != "win32",
        )
        log.close()
        deadline = time.monotonic() + _START_SECONDS
        waiting: set[int] = {port for ports in PORTS.values() for port in ports}
        while waiting:
            if self._process.poll() is not None:
                raise ServerError(
                    f"the server exited with {self._process.returncode}: see {self.results / 'server.log'}"
                )
            if time.monotonic() > deadline:
                self.stop()
                raise ServerError(f"ports {sorted(waiting)} did not answer in {_START_SECONDS:g} seconds")
            waiting = {port for port in waiting if not _answers(port)}
            time.sleep(0.1)

    def stop(self) -> None:
        """Stop the server and the processes it started. Calling it again does nothing."""
        process, self._process = self._process, None
        if process is not None and process.poll() is None:
            if sys.platform == "win32":
                subprocess.run(["taskkill", "/T", "/F", "/PID", str(process.pid)], capture_output=True, check=False)
            else:
                os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(_STOP_SECONDS)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        if self._temporary is not None:
            self._temporary.cleanup()

    def url_for(self, test: str) -> str:
        """The URL of a test, by its address from the root, as wptrunner opens it: HTTPS for a name with `.https.` and
        the like, HTTP/2 for `.h2.`, the `www` name for `.www.`, and what the query's `wpt_flags` say."""
        parts = urlsplit(f"/{test}")
        flags = set(parts.path.rsplit("/", 1)[1].split(".")[1:-1]) | set(parse_qs(parts.query).get("wpt_flags", []))
        if "h2" in flags:
            scheme, port = "https", PORTS["h2"][0]
        elif flags & {"https", "serviceworker", "serviceworker-module"}:
            scheme, port = "https", PORTS["https"][0]
        else:
            scheme, port = "http", PORTS["http"][0]
        host = f"www.{BROWSER_HOST}" if "www" in flags else BROWSER_HOST
        return f"{scheme}://{host}:{port}/{test}"

    def forget(self, test: str) -> None:
        """Delete what a test posted before."""
        (self.results / result_key(f"/{test}")).unlink(missing_ok=True)

    def posted(self, test: str) -> dict[str, Any] | None:
        """What a test has posted, or None if nothing yet."""
        try:
            return json.loads((self.results / result_key(f"/{test}")).read_bytes())
        except (FileNotFoundError, ValueError):
            # A file that the server is still writing reads as nothing yet.
            return None

    def _config(self) -> dict[str, Any]:
        return {
            "browser_host": BROWSER_HOST,
            "doc_root": str(self._root),
            "server_host": "127.0.0.1",
            "check_subdomains": False,
            "ports": PORTS,
            "aliases": [
                {"url-path": "/resources/testharnessreport.js", "local-dir": str(_REPORTER)},
                {"url-path": "/mitto/", "local-dir": str(_REPORTER)},
            ],
            "logging": {"level": self._log_level, "suppress_handler_traceback": False},
        }


def _answers(port: int) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=1):
            return True
    except OSError:
        return False
