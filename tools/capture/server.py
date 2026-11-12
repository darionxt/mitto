"""The capture server: TLS and HTTP on local TCP ports, QUIC on a local UDP port, and a recording of each port."""

import contextlib
import errno
import itertools
import logging
import selectors
import socket
import ssl
import tempfile
import threading
import time
from collections import deque
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from types import TracebackType
from typing import Protocol, Self

from aioquic.quic.connection import NetworkAddress

from tools.capture.cases import (
    CROSS_SITE_HOST,
    DONE_PATHS,
    HOST_NAMES,
    SAME_SITE_HOST,
    Run,
    respond_on,
)
from tools.capture.certificate import Certificate, generate_certificate
from tools.capture.client_hello import ClientHelloAssembler, ClientHelloRecords
from tools.capture.http1 import Http1Connection, Http1Request, parse_request_head
from tools.capture.http2 import Frame, Http2Connection, Http2Request
from tools.capture.page import DONE_PATH, RequestHead, Responder, Response
from tools.capture.quic_server import Datagram, LiveQuicConnection, QuicDispatcher
from tools.capture.recording import (
    HTTP1,
    HTTP2,
    HTTP3,
    PLAIN_HTTP,
    PROXIED_HTTP1,
    PROXIED_HTTP2,
    PROXY,
    SCRIPT,
    ConnectionRecord,
    Recording,
)
from tools.capture.script_page import SCRIPT_DONE_PATHS, ScriptRun
from tools.capture.script_page import respond as respond_on_script_route
from tools.capture.wire import ParseError

DEFAULT_HOST = "capture.localhost"
"""Chrome and the network stack resolve *.localhost to loopback themselves, and, unlike an IP address, it gets SNI."""

_HOST_SUFFIXES = (".localhost", ".test")
_MAX_CONNECT_HEAD_SIZE = 1 << 16
_TUNNEL_OPEN = b"HTTP/1.1 200 Connection established\r\n\r\n"
_TUNNEL_REFUSED = b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
_READ_SIZE = 1 << 16
_MAX_DATAGRAM_SIZE = 1 << 16
_POLL_SECONDS = 0.05
_CLOSE_TIMEOUT_SECONDS = 5.0
_LOOPBACKS = ((socket.AF_INET, "127.0.0.1"), (socket.AF_INET6, "::1"))
_NO_IPV6_ERRORS = (errno.EADDRNOTAVAIL, errno.EAFNOSUPPORT)
_BIND_ATTEMPTS = 20
# On loopback a handshake can end before a client has written a request, so what it would send early would not show.
_QUIC_DELAY_WITH_TICKETS_SECONDS = 0.1

_log = logging.getLogger(__name__)


class _HttpConnection(Protocol):
    @property
    def closed(self) -> bool: ...

    def start(self) -> bytes: ...

    def receive(self, data: bytes) -> bytes: ...


class _ConnectionClosedError(Exception):
    """The client went away before the handshake finished."""

    def __init__(self) -> None:
        super().__init__("the client closed the connection during the handshake")


class _Stream(Protocol):
    def read(self) -> bytes: ...

    def write(self, data: bytes) -> None: ...

    @property
    def alpn(self) -> str | None: ...

    @property
    def tls_version(self) -> str | None: ...

    @property
    def cipher(self) -> str | None: ...

    @property
    def resumed(self) -> bool: ...


class _LiveConnection:
    """A connection in progress. Only touch it while holding the server's lock."""

    def __init__(
        self, connection_id: int, client_hello: ClientHelloRecords | None, peer: str, connect: Http1Request | None
    ) -> None:
        self.id = connection_id
        self.client_hello = client_hello
        self.peer = peer
        self.connect = connect
        self.tls_version: str | None = None
        self.cipher: str | None = None
        self.alpn: str | None = None
        self.http: Http1Connection | Http2Connection | None = None
        self.error: str | None = None
        self.resumed = False

    def snapshot(self) -> ConnectionRecord:
        frames: tuple[Frame, ...] = ()
        requests: tuple[Http1Request | Http2Request, ...] = ()
        if isinstance(self.http, Http2Connection):
            frames = tuple(self.http.frames)
            requests = tuple(self.http.requests)
        elif isinstance(self.http, Http1Connection):
            requests = tuple(self.http.requests)
        return ConnectionRecord(
            self.id,
            self.client_hello,
            self.tls_version,
            self.cipher,
            self.alpn,
            frames,
            requests,
            self.error,
            self.peer,
            self.connect,
            self.resumed,
        )


class _Endpoint:
    """One route into the server: what it offers, and what it has recorded.

    Attributes:
        route: The route.
        requested_port: The port it was asked to listen on. 0 means any free port.
        scheme: The scheme of a URL on this route.
        port: The port a URL on this route names.
        context: What the connections on this route speak TLS with, if they do. It goes with the protocol ALPN offers.
        connections: What the route has recorded.
        done: The done paths that a client has asked for on this route.
    """

    def __init__(self, route: str, requested_port: int, scheme: str = "https") -> None:
        self.route = route
        self.requested_port = requested_port
        self.scheme = scheme
        self.port = 0
        self.context: ssl.SSLContext | None = None
        self.connections: list[_LiveConnection | LiveQuicConnection] = []
        self.done = {path: threading.Event() for path in DONE_PATHS | SCRIPT_DONE_PATHS}


@dataclass(frozen=True, slots=True)
class _Listener:
    sock: socket.socket
    endpoint: _Endpoint


@dataclass(frozen=True, slots=True)
class _QuicListener:
    sock: socket.socket


class _TlsStream:
    """TLS over a blocking socket through memory buffers, so that the server sees the bytes before TLS does."""

    def __init__(self, sock: socket.socket, context: ssl.SSLContext) -> None:
        self._sock = sock
        self._incoming = ssl.MemoryBIO()
        self._outgoing = ssl.MemoryBIO()
        self.tls = context.wrap_bio(self._incoming, self._outgoing, server_side=True)

    def feed(self, data: bytes) -> None:
        """Add bytes that were read from the socket by someone else."""
        self._incoming.write(data)

    def handshake(self) -> None:
        while True:
            try:
                self.tls.do_handshake()
            except ssl.SSLWantReadError:
                self._flush()
                if not self._fill():
                    raise _ConnectionClosedError from None
            else:
                self._flush()
                return

    def read(self) -> bytes:
        """Return the next decrypted bytes, or nothing at the end of the connection."""
        while True:
            try:
                return self.tls.read(_READ_SIZE)
            except ssl.SSLWantReadError:
                self._flush()
                if not self._fill():
                    return b""
            except ssl.SSLZeroReturnError:
                return b""

    def write(self, data: bytes) -> None:
        if data:
            self.tls.write(data)
            self._flush()

    @property
    def alpn(self) -> str | None:
        return self.tls.selected_alpn_protocol()

    @property
    def tls_version(self) -> str | None:
        return self.tls.version()

    @property
    def cipher(self) -> str | None:
        cipher = self.tls.cipher()
        return cipher[0] if cipher else None

    @property
    def resumed(self) -> bool:
        return self.tls.session_reused

    def _fill(self) -> bool:
        data = self._sock.recv(_READ_SIZE)
        if not data:
            return False
        self._incoming.write(data)
        return True

    def _flush(self) -> None:
        data = self._outgoing.read()
        if data:
            self._sock.sendall(data)


class _PlainStream:
    """A connection without TLS."""

    alpn = None
    tls_version = None
    cipher = None
    resumed = False

    def __init__(self, sock: socket.socket) -> None:
        self._sock = sock

    def read(self) -> bytes:
        return self._sock.recv(_READ_SIZE)

    def write(self, data: bytes) -> None:
        if data:
            self._sock.sendall(data)


class CaptureServer:
    """Serves the test pages and records what clients send, one recording per route into the server.

    HTTP/1.1, HTTP/2 and HTTP/3 have a port each. Plain HTTP has a port, and so does an HTTP proxy, through which HTTP/1.1
    and HTTP/2 can be reached too. The Script Fingerprint's pages have a port of their own, over HTTP/1.1. Use it as a
    context manager. Clients must connect by name, so that they send SNI.
    Unless asked to, it gives out no session tickets, so every ClientHello is that of a fresh connection and has no
    pre_shared_key.
    """

    def __init__(
        self,
        host: str = DEFAULT_HOST,
        *,
        http1_port: int = 0,
        http2_port: int = 0,
        http3_port: int = 0,
        plain_http_port: int = 0,
        proxy_port: int = 0,
        script_port: int = 0,
        tickets: bool = False,
    ) -> None:
        """Prepare a server at host, which must end in .localhost or .test.

        The server also answers under the names of the case pages, which only a client told how to resolve them finds.

        A port of 0 means any free port whose number no other port of this server has. Named ports may share a number
        across TCP and UDP.

        With tickets, connections over TLS get session tickets, so that a client's later connections can resume. Those
        over HTTP/3 allow early data, and those over TCP do not, because Python's ssl has no way to say that they do.
        Datagrams then wait a moment before the server reads them, as if it were far away, which is what gives a client
        time to send early data.
        """
        if not host.endswith(_HOST_SUFFIXES):
            raise ValueError(f"{host!r} does not end in .localhost or .test")
        self.host = host
        self._tickets = tickets
        self._certificate: Certificate | None = None
        self._endpoints = {
            HTTP1: _Endpoint(HTTP1, http1_port),
            HTTP2: _Endpoint(HTTP2, http2_port),
            HTTP3: _Endpoint(HTTP3, http3_port),
            PLAIN_HTTP: _Endpoint(PLAIN_HTTP, plain_http_port, "http"),
            PROXY: _Endpoint(PROXY, proxy_port, "http"),
            PROXIED_HTTP1: _Endpoint(PROXIED_HTTP1, 0),
            PROXIED_HTTP2: _Endpoint(PROXIED_HTTP2, 0),
            SCRIPT: _Endpoint(SCRIPT, script_port),
        }
        self._lock = threading.Lock()
        self._connection_ids = itertools.count(1)
        self._selector = selectors.DefaultSelector()
        self._listeners: list[socket.socket] = []
        self._quic: QuicDispatcher | None = None
        self._quic_sockets: dict[int, socket.socket] = {}
        self._held_datagrams: deque[tuple[float, bytes, NetworkAddress]] = deque()
        self._stopping = threading.Event()
        self._listen_thread: threading.Thread | None = None
        self._connection_threads: list[threading.Thread] = []
        self._open_sockets: set[socket.socket] = set()

    def __enter__(self) -> Self:
        self.start()
        return self

    def __exit__(
        self, exc_type: type[BaseException] | None, exc: BaseException | None, traceback: TracebackType | None
    ) -> None:
        self.close()

    @property
    def spki_hash(self) -> str:
        """The value for Chrome's --ignore-certificate-errors-spki-list."""
        if self._certificate is None:
            raise RuntimeError("the server has not started")
        return self._certificate.spki_hash

    @property
    def certificate_pem(self) -> str:
        """The self-signed certificate that every port presents, which a client can add as a trust anchor."""
        if self._certificate is None:
            raise RuntimeError("the server has not started")
        return self._certificate.certificate_pem.decode()

    @property
    def host_resolver_rules(self) -> str:
        """The value for Chrome's --host-resolver-rules, and the like, which sends the case pages' names here."""
        return ",".join(f"MAP {name} {_LOOPBACKS[0][1]}" for name in HOST_NAMES)

    @property
    def proxy_url(self) -> str:
        """The value for Chrome's --proxy-server."""
        return f"http://{_LOOPBACKS[0][1]}:{self._endpoints[PROXY].port}"

    def url(self, route: str, *, host: str | None = None, path: str = "/") -> str:
        """The address of path on a route, at host or else at the server's own name."""
        return f"{self._endpoints[route].scheme}://{self.authority(route, host=host)}{path}"

    def authority(self, route: str, *, host: str | None = None) -> str:
        """The name and port of a route, at host or else at the server's own name."""
        return f"{host or self.host}:{self.port(route)}"

    def port(self, route: str) -> int:
        """The port a URL on a route names."""
        return self._endpoints[route].port

    def recording(self, route: str, *, host: str | None = None) -> Recording:
        """Return what a route has recorded so far, as seen from a page at host or else at the server's own name."""
        endpoint = self._endpoints[route]
        with self._lock:
            connections = tuple(connection.snapshot() for connection in endpoint.connections)
        return Recording(
            host or self.host,
            self.authority(route, host=host),
            connections,
            self.authority(route, host=SAME_SITE_HOST),
            self.authority(route, host=CROSS_SITE_HOST),
            endpoint.scheme,
        )

    def wait_for_page(self, route: str, timeout: float, *, done_path: str = DONE_PATH) -> bool:
        """Wait until a page has finished loading on a route, which it says at done_path. Returns whether it has."""
        return self._endpoints[route].done[done_path].wait(timeout)

    def start_url(self, run: Run | ScriptRun) -> str:
        """The address a run starts at."""
        return self.url(run.route, host=run.host, path=run.start_path)

    def chrome_flags(self, run: Run | ScriptRun) -> list[str]:
        """The flags a Chrome needs for a run, on top of those for any Chrome."""
        flags = [f"--ignore-certificate-errors-spki-list={self.spki_hash}"]
        if run.route in {PROXIED_HTTP1, PROXIED_HTTP2}:
            # The proxy is what finds the name, so nothing here says how to resolve it.
            return [*flags, f"--proxy-server={self.proxy_url}"]
        if run.host is not None:
            # A proxy of this machine's would get these names, where it would never get a *.localhost name.
            flags += [f"--host-resolver-rules={self.host_resolver_rules}", "--no-proxy-server"]
        if run.route == HTTP3:
            flags.append(f"--origin-to-force-quic-on={','.join(self.origins_to_force_quic_on(run))}")
        return flags

    def origins_to_force_quic_on(self, run: Run | ScriptRun) -> list[str]:
        """The origins a client has to be told speak QUIC for a run, each written "host:port"."""
        if run.route != HTTP3:
            return []
        hosts = (run.host,) if run.host is None else HOST_NAMES
        return [self.authority(HTTP3, host=host) for host in hosts]

    def start(self) -> None:
        """Start listening. A server starts only once.

        Raises:
            OSError: A port is not free.
            RuntimeError: The server has started before.
        """
        if self._certificate is not None:
            raise RuntimeError("the server has started before")
        try:
            self._start()
        except BaseException:
            self.close()
            raise

    def close(self) -> None:
        """Stop serving and end all connections. Recordings stay readable."""
        self._stopping.set()
        deadline = time.monotonic() + _CLOSE_TIMEOUT_SECONDS
        if self._listen_thread is not None:
            self._listen_thread.join(_CLOSE_TIMEOUT_SECONDS)
            self._listen_thread = None
        with self._lock:
            open_sockets = list(self._open_sockets)
        for sock in open_sockets:
            # This wakes the thread blocked on the socket, on every operating system.
            with contextlib.suppress(OSError):
                sock.shutdown(socket.SHUT_RDWR)
        for thread in self._connection_threads:
            thread.join(max(0.0, deadline - time.monotonic()))
        self._connection_threads.clear()
        for listener in self._listeners:
            listener.close()
        self._listeners.clear()
        self._selector.close()

    def _start(self) -> None:
        self._certificate = generate_certificate(self.host, *(name for name in HOST_NAMES if name != self.host))
        for protocol, proxied in ((HTTP1, PROXIED_HTTP1), (HTTP2, PROXIED_HTTP2)):
            direct = self._endpoints[protocol]
            direct.context = _server_context(self._certificate, protocol, tickets=self._tickets)
            self._listen(direct)
            tunnel = self._endpoints[proxied]
            tunnel.context = direct.context
            tunnel.port = direct.port
        script = self._endpoints[SCRIPT]
        script.context = _server_context(self._certificate, HTTP1, tickets=self._tickets)
        self._listen(script)
        self._listen(self._endpoints[PLAIN_HTTP])
        self._listen(self._endpoints[PROXY])
        self._listen_quic(self._endpoints[HTTP3], self._certificate)
        self._listen_thread = threading.Thread(target=self._listen_loop, daemon=True)
        self._listen_thread.start()

    def _listen(self, endpoint: _Endpoint) -> None:
        for sock in self._bind(endpoint, _open_tcp):
            self._selector.register(sock, selectors.EVENT_READ, _Listener(sock, endpoint))

    def _listen_quic(self, endpoint: _Endpoint, certificate: Certificate) -> None:
        def record(connection: LiveQuicConnection) -> None:
            connection.id = next(self._connection_ids)
            endpoint.connections.append(connection)

        self._quic = QuicDispatcher(certificate, _page_responder(endpoint), record, tickets=self._tickets)
        for sock in self._bind(endpoint, _open_udp):
            self._quic_sockets[sock.family] = sock
            self._selector.register(sock, selectors.EVENT_READ, _QuicListener(sock))

    def _bind(
        self, endpoint: _Endpoint, open_socket: Callable[[socket.AddressFamily, str, int], socket.socket]
    ) -> list[socket.socket]:
        # TCP and UDP number their ports apart, so port 0 may pick the number of a port of the other kind. A client
        # speaking TCP to the HTTP/3 port would then reach another port's listener instead of being refused.
        others = {other.port or other.requested_port for other in self._endpoints.values() if other is not endpoint}
        for _ in range(_BIND_ATTEMPTS):
            try:
                sockets = _bind_loopback(endpoint.requested_port, open_socket)
            except OSError as error:
                # Port 0 may pick a port that is free on one loopback address only.
                if endpoint.requested_port == 0 and error.errno == errno.EADDRINUSE:
                    continue
                raise
            port = sockets[0].getsockname()[1]
            if endpoint.requested_port == 0 and port in others:
                for sock in sockets:
                    sock.close()
                continue
            endpoint.port = port
            for sock in sockets:
                sock.setblocking(False)
            self._listeners += sockets
            return sockets
        raise OSError(errno.EADDRINUSE, "no port free on all loopback addresses")

    def _listen_loop(self) -> None:
        while not self._stopping.is_set():
            for key, _ in self._selector.select(self._poll_seconds()):
                listener: _Listener | _QuicListener = key.data
                if isinstance(listener, _QuicListener):
                    self._receive_datagram(listener.sock)
                else:
                    self._accept(listener)
            self._handle_quic_timers()

    def _accept(self, listener: _Listener) -> None:
        try:
            sock, address = listener.sock.accept()
        except OSError:
            return
        sock.setblocking(True)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        with self._lock:
            self._open_sockets.add(sock)
        thread = threading.Thread(target=self._serve, args=(sock, address[0], listener), daemon=True)
        self._connection_threads.append(thread)
        thread.start()

    def _poll_seconds(self) -> float:
        assert self._quic is not None
        with self._lock:
            wakeup = self._quic.next_wakeup()
        wakeups = [wakeup] if wakeup is not None else []
        if self._held_datagrams:
            wakeups.append(self._held_datagrams[0][0])
        return min([_POLL_SECONDS, *(max(0.0, when - time.monotonic()) for when in wakeups)])

    def _receive_datagram(self, sock: socket.socket) -> None:
        assert self._quic is not None
        try:
            data, address = sock.recvfrom(_MAX_DATAGRAM_SIZE)
        except OSError:
            # Windows reports here that an earlier datagram found nobody listening. This one is not lost.
            return
        if self._tickets:
            self._held_datagrams.append((time.monotonic() + _QUIC_DELAY_WITH_TICKETS_SECONDS, data, address))
            return
        self._read_datagram(data, address)

    def _read_datagram(self, data: bytes, address: NetworkAddress) -> None:
        assert self._quic is not None
        with self._lock:
            output = self._quic.receive(data, address, time.monotonic())
        self._send_datagrams(output)

    def _handle_quic_timers(self) -> None:
        assert self._quic is not None
        while self._held_datagrams and self._held_datagrams[0][0] <= time.monotonic():
            _, data, address = self._held_datagrams.popleft()
            self._read_datagram(data, address)
        with self._lock:
            output = self._quic.handle_timers(time.monotonic())
        self._send_datagrams(output)

    def _send_datagrams(self, datagrams: list[Datagram]) -> None:
        for data, address in datagrams:
            family = socket.AF_INET6 if ":" in address[0] else socket.AF_INET
            # A datagram that goes nowhere is the same as one that gets lost, which QUIC handles.
            with contextlib.suppress(OSError):
                self._quic_sockets[family].sendto(data, address)

    def _serve(self, sock: socket.socket, peer: str, listener: _Listener) -> None:
        live: _LiveConnection | None = None
        try:
            endpoint = listener.endpoint
            connect = None
            if endpoint.route == PROXY:
                endpoint, connect = self._open_tunnel(sock)
                if endpoint is None:
                    return
            stream: _Stream
            if endpoint.context is None:
                stream = _PlainStream(sock)
                live = self._record_connection(endpoint, None, peer, None)
            else:
                tls_stream = _TlsStream(sock, endpoint.context)
                live = self._read_client_hello(sock, peer, tls_stream, endpoint, connect)
                tls_stream.handshake()
                stream = tls_stream
            http = self._start_http(live, stream, endpoint)
            stream.write(http.start())
            while not http.closed:
                data = stream.read()
                if not data:
                    break
                with self._lock:
                    output = http.receive(data)
                stream.write(output)
        except (OSError, ParseError, _ConnectionClosedError) as error:
            description = f"{type(error).__name__}: {error}"
            if live is None:
                # Nothing recorded this connection, so this is the only trace of it.
                _log.warning("gave up on a connection before its ClientHello was complete: %s", description)
            else:
                with self._lock:
                    live.error = description
        finally:
            with self._lock:
                self._open_sockets.discard(sock)
            sock.close()

    def _open_tunnel(self, sock: socket.socket) -> tuple[_Endpoint | None, Http1Request | None]:
        """Read what a client asks of the proxy and answer it. Returns the route of the tunnel, if it gets one."""
        head = bytearray()
        while (end := head.find(b"\r\n\r\n")) < 0:
            if len(head) > _MAX_CONNECT_HEAD_SIZE:
                raise ParseError(f"request head over {_MAX_CONNECT_HEAD_SIZE} bytes")
            data = sock.recv(_READ_SIZE)
            if not data:
                raise _ConnectionClosedError
            head += data
        if len(head) > end + 4:
            raise ParseError("bytes that did not wait for an answer to CONNECT")
        request = parse_request_head(bytes(head[:end]))
        endpoint = self._tunnel_endpoint(request) if request.method == "CONNECT" else None
        if endpoint is None:
            # Chrome's other traffic comes this way too, and none of it may leave this machine.
            sock.sendall(_TUNNEL_REFUSED)
            return None, None
        sock.sendall(_TUNNEL_OPEN)
        return endpoint, request

    def _tunnel_endpoint(self, request: Http1Request) -> _Endpoint | None:
        name, _, port = request.target.rpartition(":")
        if name != self.host and name not in HOST_NAMES:
            return None
        for direct, proxied in ((HTTP1, PROXIED_HTTP1), (HTTP2, PROXIED_HTTP2)):
            if port == str(self._endpoints[direct].port):
                return self._endpoints[proxied]
        return None

    def _record_connection(
        self,
        endpoint: _Endpoint,
        client_hello: ClientHelloRecords | None,
        peer: str,
        connect: Http1Request | None,
    ) -> _LiveConnection:
        with self._lock:
            live = _LiveConnection(next(self._connection_ids), client_hello, peer, connect)
            endpoint.connections.append(live)
        return live

    def _read_client_hello(
        self, sock: socket.socket, peer: str, stream: _TlsStream, endpoint: _Endpoint, connect: Http1Request | None
    ) -> _LiveConnection:
        assembler = ClientHelloAssembler()
        while True:
            data = sock.recv(_READ_SIZE)
            if not data:
                raise _ConnectionClosedError
            stream.feed(data)
            client_hello = assembler.feed(data)
            if client_hello is not None:
                return self._record_connection(endpoint, client_hello, peer, connect)

    def _start_http(self, live: _LiveConnection, stream: _Stream, endpoint: _Endpoint) -> _HttpConnection:
        alpn = stream.alpn
        responder = _page_responder(endpoint)
        http = Http2Connection(responder) if alpn == HTTP2 else Http1Connection(responder)
        with self._lock:
            live.tls_version = stream.tls_version
            live.cipher = stream.cipher
            live.alpn = alpn
            live.http = http
            live.resumed = stream.resumed
        return http


def _page_responder(endpoint: _Endpoint) -> Responder:
    def answer(request: RequestHead) -> Response:
        done = endpoint.done.get(request.target)
        if done is not None:
            done.set()
        if endpoint.route == SCRIPT:
            return respond_on_script_route(request)
        return respond_on(endpoint.scheme, endpoint.port, request)

    return answer


def _server_context(certificate: Certificate, protocol: str, *, tickets: bool) -> ssl.SSLContext:
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    if not tickets:
        # With no tickets to resume from, a client's later connections look like its first.
        context.num_tickets = 0
        context.options |= ssl.OP_NO_TICKET
    context.set_alpn_protocols([protocol])
    with tempfile.TemporaryDirectory() as directory:
        certificate_path = Path(directory) / "certificate.pem"
        key_path = Path(directory) / "key.pem"
        certificate_path.write_bytes(certificate.certificate_pem)
        key_path.write_bytes(certificate.private_key_pem)
        context.load_cert_chain(certificate_path, key_path)
    return context


def _open_tcp(family: socket.AddressFamily, address: str, port: int) -> socket.socket:
    return socket.create_server((address, port), family=family)


def _open_udp(family: socket.AddressFamily, address: str, port: int) -> socket.socket:
    sock = socket.socket(family, socket.SOCK_DGRAM)
    try:
        sock.bind((address, port))
    except BaseException:
        sock.close()
        raise
    return sock


def _bind_loopback(
    port: int, open_socket: Callable[[socket.AddressFamily, str, int], socket.socket]
) -> list[socket.socket]:
    """Listen on port on each loopback address the machine has, the first one choosing the port if port is 0."""
    sockets: list[socket.socket] = []
    try:
        for family, address in _LOOPBACKS:
            try:
                sock = open_socket(family, address, port)
            except OSError as error:
                if family == socket.AF_INET6 and error.errno in _NO_IPV6_ERRORS:
                    continue
                raise
            sockets.append(sock)
            port = sock.getsockname()[1]
    except BaseException:
        for sock in sockets:
            sock.close()
        raise
    return sockets
