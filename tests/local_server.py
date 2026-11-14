"""The base of the local HTTP servers that tests start, and local ports that no other test can take."""

import http.server
import os
import random
import socket
import socketserver
import sys
from typing import Any

# The operating system gives a bind of port 0 one from 32768 up on Linux, and from 49152 up on macOS and Windows, so only a
# bind to its own number takes a port below them. The ports that Chrome refuses as unsafe all lie below 10081.
_PORTS_NOT_GIVEN_OUT = range(20000, 32768)


def unused_ports(count: int, *kinds: socket.SocketKind) -> list[int]:
    """Return different local ports that each kind of socket can bind now, and that no other test can get meanwhile.

    A port that a socket bound to port 0 had is free, once the socket closes, for the next bind of port 0, maybe in a test
    of another pytest-xdist worker. These come from below every operating system's range for port 0, from a slice that
    this worker has to itself.

    Args:
        count: How many ports.
        kinds: The kinds of socket that must each be able to bind every port, TCP's alone if none.

    Raises:
        RuntimeError: Fewer ports than that are free in this worker's slice.
    """
    workers = int(os.environ.get("PYTEST_XDIST_WORKER_COUNT", "1"))
    worker = int(os.environ.get("PYTEST_XDIST_WORKER", "gw0").removeprefix("gw"))
    size = len(_PORTS_NOT_GIVEN_OUT) // workers
    slice_of_worker = _PORTS_NOT_GIVEN_OUT[worker * size : (worker + 1) * size]
    ports: list[int] = []
    for port in random.sample(slice_of_worker, len(slice_of_worker)):
        if all(_binds(port, kind) for kind in kinds or (socket.SOCK_STREAM,)):
            ports.append(port)
            if len(ports) == count:
                return ports
    raise RuntimeError(f"only {len(ports)} of {count} ports are free in {slice_of_worker}")


def refused_port() -> int:
    """Return a local port that nothing listens on, and that no server another test starts can get.

    A socket bound to a port and kept from listening would not do: Linux and Windows refuse a connection to it, but macOS
    lets the attempt time out.
    """
    return unused_ports(1)[0]


def _binds(port: int, kind: socket.SocketKind) -> bool:
    """Return whether a socket of a kind can bind a local port now."""
    with socket.socket(socket.AF_INET, kind) as probe:
        if sys.platform == "win32":
            # Without it, Windows lets this bind share the port with a socket bound to any address.
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        try:
            probe.bind(("127.0.0.1", port))
        except OSError:
            return False
    return True


class LocalHTTPServer(http.server.ThreadingHTTPServer):
    """A ThreadingHTTPServer that does not look up a host name for its address.

    HTTPServer.server_bind() does a reverse DNS lookup to set server_name, which only CGI handlers read. The first
    lookup in a process can wait about 35 seconds for mDNS on a macOS CI runner.

    Call shutdown() before leaving its with statement, if a thread runs serve_forever(). That thread would otherwise go on
    polling the number of the closed socket. On Linux, a poll holds on to the socket that has that number by then, so a test
    that closes a listener to get a port that refuses connections finds it still taking them until the poll ends.
    """

    def server_bind(self) -> None:
        socketserver.TCPServer.server_bind(self)
        self.server_port = self.server_address[1]


class QuietServer(LocalHTTPServer):
    """A local server whose handlers can outlive it, and whose clients may drop a connection at any point."""

    daemon_threads = True

    def handle_error(self, request: Any, client_address: Any) -> None:
        pass
