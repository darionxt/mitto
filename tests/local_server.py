"""The base of the local HTTP servers that tests start."""

import http.server
import socketserver
from typing import Any


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
