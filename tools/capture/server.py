"""The capture server: TLS and HTTP on local ports, with a recording of each port's traffic."""

import contextlib
import errno
import logging
import selectors
import socket
import ssl
import tempfile
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from types import TracebackType
from typing import Protocol, Self

from tools.capture.certificate import Certificate, generate_certificate
from tools.capture.client_hello import ClientHelloAssembler, ClientHelloRecords
from tools.capture.http1 import Http1Connection, Http1Request
from tools.capture.http2 import Frame, Http2Connection, Http2Request
from tools.capture.page import DONE_PATH, Responder, Response, respond
from tools.capture.recording import HTTP1, HTTP2, ConnectionRecord, Recording
from tools.capture.wire import ParseError

DEFAULT_HOST = "capture.localhost"
"""Chrome and the network stack resolve *.localhost to loopback themselves, and, unlike an IP address, it gets SNI."""

_READ_SIZE = 1 << 16
_ACCEPT_POLL_SECONDS = 0.05
_CLOSE_TIMEOUT_SECONDS = 5.0
_LOOPBACKS = ((socket.AF_INET, "127.0.0.1"), (socket.AF_INET6, "::1"))
_NO_IPV6_ERRORS = (errno.EADDRNOTAVAIL, errno.EAFNOSUPPORT)
_BIND_ATTEMPTS = 20

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


class _LiveConnection:
    """A connection in progress. Only touch it while holding the server's lock."""

    def __init__(self, connection_id: int, client_hello: ClientHelloRecords) -> None:
        self.id = connection_id
        self.client_hello = client_hello
        self.tls_version: str | None = None
        self.cipher: str | None = None
        self.alpn: str | None = None
        self.http: Http1Connection | Http2Connection | None = None
        self.error: str | None = None

    def snapshot(self) -> ConnectionRecord:
        frames: tuple[Frame, ...] = ()
        requests: tuple[Http1Request | Http2Request, ...] = ()
        if isinstance(self.http, Http2Connection):
            frames = tuple(self.http.frames)
            requests = tuple(self.http.requests)
        elif isinstance(self.http, Http1Connection):
            requests = tuple(self.http.requests)
        return ConnectionRecord(
            self.id, self.client_hello, self.tls_version, self.cipher, self.alpn, frames, requests, self.error
        )


class _Endpoint:
    """One port: the protocol it offers through ALPN, and what it has recorded."""

    def __init__(self, protocol: str, requested_port: int) -> None:
        self.protocol = protocol
        self.requested_port = requested_port
        self.port = 0
        self.connections: list[_LiveConnection] = []
        self.page_done = threading.Event()


@dataclass(frozen=True, slots=True)
class _Listener:
    sock: socket.socket
    endpoint: _Endpoint
    context: ssl.SSLContext


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


class CaptureServer:
    """Serves the test page over HTTP/1.1 and HTTP/2, one port each, and records what clients send to each port.

    Use it as a context manager. Clients must connect by name, so that they send SNI. Connections never get session
    tickets, so every ClientHello is that of a fresh connection and has no pre_shared_key.
    """

    def __init__(self, host: str = DEFAULT_HOST, *, http1_port: int = 0, http2_port: int = 0) -> None:
        """Prepare a server at host, which must end in .localhost. A port of 0 means any free port."""
        if not host.endswith(".localhost"):
            raise ValueError(f"{host!r} does not end in .localhost")
        self.host = host
        self._certificate: Certificate | None = None
        self._endpoints = {HTTP1: _Endpoint(HTTP1, http1_port), HTTP2: _Endpoint(HTTP2, http2_port)}
        self._lock = threading.Lock()
        self._next_connection_id = 1
        self._selector = selectors.DefaultSelector()
        self._listeners: list[socket.socket] = []
        self._stopping = threading.Event()
        self._accept_thread: threading.Thread | None = None
        self._connection_threads: list[threading.Thread] = []
        self._open_sockets: set[socket.socket] = set()

    def __enter__(self) -> Self:
        try:
            self._start()
        except BaseException:
            self.close()
            raise
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

    def url(self, protocol: str) -> str:
        """The test page's address on the port for protocol."""
        return f"https://{self.authority(protocol)}/"

    def authority(self, protocol: str) -> str:
        """The name and port of the port for protocol."""
        return f"{self.host}:{self.port(protocol)}"

    def port(self, protocol: str) -> int:
        """The port for protocol."""
        return self._endpoints[protocol].port

    def recording(self, protocol: str) -> Recording:
        """Return what the port for protocol has recorded so far."""
        endpoint = self._endpoints[protocol]
        with self._lock:
            connections = tuple(connection.snapshot() for connection in endpoint.connections)
        return Recording(self.host, self.authority(protocol), connections)

    def wait_for_page(self, protocol: str, timeout: float) -> bool:
        """Wait until the test page has finished loading on the port for protocol. Returns whether it has."""
        return self._endpoints[protocol].page_done.wait(timeout)

    def close(self) -> None:
        """Stop serving and end all connections. Recordings stay readable."""
        self._stopping.set()
        deadline = time.monotonic() + _CLOSE_TIMEOUT_SECONDS
        if self._accept_thread is not None:
            self._accept_thread.join(_CLOSE_TIMEOUT_SECONDS)
            self._accept_thread = None
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
        self._certificate = generate_certificate(self.host)
        for endpoint in self._endpoints.values():
            self._listen(endpoint, _server_context(self._certificate, endpoint.protocol))
        self._accept_thread = threading.Thread(target=self._accept_loop, daemon=True)
        self._accept_thread.start()

    def _listen(self, endpoint: _Endpoint, context: ssl.SSLContext) -> None:
        for _ in range(_BIND_ATTEMPTS):
            try:
                sockets = _bind_loopback(endpoint.requested_port)
            except OSError as error:
                # Port 0 may pick a port that is free on one loopback address only.
                if endpoint.requested_port == 0 and error.errno == errno.EADDRINUSE:
                    continue
                raise
            endpoint.port = sockets[0].getsockname()[1]
            for sock in sockets:
                sock.setblocking(False)
                self._selector.register(sock, selectors.EVENT_READ, _Listener(sock, endpoint, context))
            self._listeners += sockets
            return
        raise OSError(errno.EADDRINUSE, "no port free on all loopback addresses")

    def _accept_loop(self) -> None:
        while not self._stopping.is_set():
            for key, _ in self._selector.select(_ACCEPT_POLL_SECONDS):
                listener: _Listener = key.data
                try:
                    sock, _ = listener.sock.accept()
                except OSError:
                    continue
                sock.setblocking(True)
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                with self._lock:
                    self._open_sockets.add(sock)
                thread = threading.Thread(target=self._serve, args=(sock, listener), daemon=True)
                self._connection_threads.append(thread)
                thread.start()

    def _serve(self, sock: socket.socket, listener: _Listener) -> None:
        live: _LiveConnection | None = None
        try:
            stream = _TlsStream(sock, listener.context)
            live = self._read_client_hello(sock, stream, listener.endpoint)
            stream.handshake()
            http = self._start_http(live, stream, listener.endpoint)
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

    def _read_client_hello(self, sock: socket.socket, stream: _TlsStream, endpoint: _Endpoint) -> _LiveConnection:
        assembler = ClientHelloAssembler()
        while True:
            data = sock.recv(_READ_SIZE)
            if not data:
                raise _ConnectionClosedError
            stream.feed(data)
            client_hello = assembler.feed(data)
            if client_hello is not None:
                with self._lock:
                    live = _LiveConnection(self._next_connection_id, client_hello)
                    self._next_connection_id += 1
                    endpoint.connections.append(live)
                return live

    def _start_http(self, live: _LiveConnection, stream: _TlsStream, endpoint: _Endpoint) -> _HttpConnection:
        alpn = stream.tls.selected_alpn_protocol()
        cipher = stream.tls.cipher()
        responder = _page_responder(endpoint)
        http = Http2Connection(responder) if alpn == HTTP2 else Http1Connection(responder)
        with self._lock:
            live.tls_version = stream.tls.version()
            live.cipher = cipher[0] if cipher else None
            live.alpn = alpn
            live.http = http
        return http


def _page_responder(endpoint: _Endpoint) -> Responder:
    def answer(target: str) -> Response:
        if target == DONE_PATH:
            endpoint.page_done.set()
        return respond(target)

    return answer


def _server_context(certificate: Certificate, protocol: str) -> ssl.SSLContext:
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
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


def _bind_loopback(port: int) -> list[socket.socket]:
    """Listen on port on each loopback address the machine has, the first one choosing the port if port is 0."""
    sockets: list[socket.socket] = []
    try:
        for family, address in _LOOPBACKS:
            try:
                sock = socket.create_server((address, port), family=family)
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
