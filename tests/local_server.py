"""The base of the local HTTP servers that tests start."""

import http.server
import socketserver


class LocalHTTPServer(http.server.ThreadingHTTPServer):
    """A ThreadingHTTPServer that does not look up a host name for its address.

    HTTPServer.server_bind() does a reverse DNS lookup to set server_name, which only CGI handlers read. The first
    lookup in a process can wait about 35 seconds for mDNS on a macOS CI runner.
    """

    def server_bind(self) -> None:
        socketserver.TCPServer.server_bind(self)
        self.server_port = self.server_address[1]
