"""A proxy on loopback for tests, which speaks HTTP, HTTP inside TLS or SOCKS5.

It tunnels to the port that a client names on this machine, whatever name goes with that port, so that a client can ask
for a name that exists nowhere, which //net does not take around a proxy.
"""

import select
import socket
import socketserver
import ssl
from dataclasses import dataclass
from pathlib import Path
from typing import cast

from cryptography.hazmat.primitives import serialization

from tests.local_certificates import Issued
from tests.local_server import QuietServer

_LOOPBACK = "127.0.0.1"
_READ_SIZE = 64 * 1024
_TUNNEL_OPEN = b"HTTP/1.1 200 Connection established\r\n\r\n"
_BODY = b"answered by the proxy"

_SOCKS_VERSION = 5
_SOCKS_CONNECT = 1
_SOCKS_NO_AUTHENTICATION = 0
_SOCKS_NAME = 3
_SOCKS_SUCCEEDED = b"\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00"

BASIC_CHALLENGE = 'Basic realm="mitto"'
DIGEST_CHALLENGE = 'Digest realm="mitto", nonce="abc", qop="auth"'


@dataclass(frozen=True, slots=True)
class ProxiedRequest:
    """What a client asked of the proxy over HTTP.

    Attributes:
        request_line: Its first line, such as "CONNECT example.test:443 HTTP/1.1".
        headers: The (name, value) pairs in the order received.
    """

    request_line: str
    headers: tuple[tuple[str, str], ...]

    def header(self, name: str) -> str | None:
        """Return the value of the first header of this name, in this spelling, or None if there is none."""
        return next((value for key, value in self.headers if key == name), None)

    @property
    def target(self) -> str:
        """The part of the first line between the method and the version."""
        return self.request_line.split(" ")[1]


class _ProxyHandler(socketserver.StreamRequestHandler):
    def handle(self) -> None:
        proxy = cast("LocalProxy", self.server)
        if proxy.socks:
            self._handle_socks()
            return
        while self._handle_http(proxy):
            pass

    def _handle_http(self, proxy: "LocalProxy") -> bool:
        """Answer one request, and return whether another may follow on this connection."""
        request = self._read_request()
        if request is None:
            return False
        proxy.requests.append(request)
        if proxy.needs_challenge(request):
            self.wfile.write(
                b"HTTP/1.1 407 Proxy Authentication Required\r\n"
                + f"Proxy-Authenticate: {proxy.challenge}\r\n".encode()
                + b"Content-Length: 0\r\n\r\n"
            )
            return True
        if request.request_line.startswith("CONNECT "):
            self.wfile.write(_TUNNEL_OPEN)
            self._tunnel(int(request.target.rpartition(":")[2]))
            return False
        location = proxy.redirects.get(request.target)
        if location is not None:
            self.wfile.write(f"HTTP/1.1 302 Found\r\nLocation: {location}\r\nContent-Length: 0\r\n\r\n".encode())
            return True
        self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n%s" % (len(_BODY), _BODY))
        return True

    def _read_request(self) -> ProxiedRequest | None:
        request_line = self.rfile.readline().decode("latin-1").rstrip("\r\n")
        if not request_line:
            return None
        headers: list[tuple[str, str]] = []
        while line := self.rfile.readline().decode("latin-1").rstrip("\r\n"):
            name, _, value = line.partition(": ")
            headers.append((name, value))
        return ProxiedRequest(request_line, tuple(headers))

    def _handle_socks(self) -> None:
        proxy = cast("LocalProxy", self.server)
        version, method_count = self.rfile.read(2)
        methods = self.rfile.read(method_count)
        assert version == _SOCKS_VERSION
        assert _SOCKS_NO_AUTHENTICATION in methods
        self.wfile.write(bytes((_SOCKS_VERSION, _SOCKS_NO_AUTHENTICATION)))
        version, command, _, address_type = self.rfile.read(4)
        assert (version, command, address_type) == (_SOCKS_VERSION, _SOCKS_CONNECT, _SOCKS_NAME)
        name = self.rfile.read(self.rfile.read(1)[0]).decode()
        port = int.from_bytes(self.rfile.read(2))
        proxy.socks_targets.append(f"{name}:{port}")
        self.wfile.write(_SOCKS_SUCCEEDED)
        self._tunnel(port)

    def _tunnel(self, port: int) -> None:
        with socket.create_connection((_LOOPBACK, port)) as target:
            sockets = [self.connection, target]
            while True:
                readable, _, _ = select.select(sockets, [], [])
                for source in readable:
                    data = source.recv(_READ_SIZE)
                    if not data:
                        return
                    (target if source is self.connection else self.connection).sendall(data)


class LocalProxy(QuietServer):
    """A proxy on loopback. Use it with serving().

    Attributes:
        socks: Whether it speaks SOCKS5 and not HTTP.
        authorization: What Proxy-Authorization has to hold, or None if it asks for nothing. An empty string takes any.
        challenge: What Proxy-Authenticate holds when it asks.
        challenge_each_host: Whether it asks once for each host, whatever a client sends first.
        redirects: For a plain HTTP request, the Location to answer each target with. It answers any other with a 200.
        requests: What it has been asked over HTTP.
        socks_targets: The "name:port" of each SOCKS5 tunnel.
    """

    def __init__(
        self,
        *,
        socks: bool = False,
        authorization: str | None = None,
        challenge: str = BASIC_CHALLENGE,
        challenge_each_host: bool = False,
    ) -> None:
        super().__init__((_LOOPBACK, 0), _ProxyHandler)
        self.socks = socks
        self.authorization = authorization
        self.challenge = challenge
        self.challenge_each_host = challenge_each_host
        self.redirects: dict[str, str] = {}
        self.requests: list[ProxiedRequest] = []
        self.socks_targets: list[str] = []
        self._hosts_asked: set[str] = set()

    @property
    def port(self) -> int:
        return self.server_port

    def use_tls(self, directory: Path, leaf: Issued) -> None:
        """Speak inside TLS from now on, presenting `leaf`, and offer only HTTP/1.1 there."""
        key = leaf.key.private_bytes(
            serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()
        ).decode()
        chain_file = directory / f"proxy-{self.port}.pem"
        chain_file.write_text(leaf.pem + key)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(chain_file)
        context.set_alpn_protocols(["http/1.1"])
        self.socket = context.wrap_socket(self.socket, server_side=True)

    def needs_challenge(self, request: ProxiedRequest) -> bool:
        """Return whether to answer a request with a 407, and remember what that takes."""
        if self.authorization is None:
            return False
        host = request.header("Host") or ""
        if self.challenge_each_host and host not in self._hosts_asked:
            self._hosts_asked.add(host)
            return True
        given = request.header("Proxy-Authorization")
        return given is None or (bool(self.authorization) and given != self.authorization)
