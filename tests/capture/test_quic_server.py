"""Tests of the QUIC side of the capture server with no sockets and a clock of their own, with aioquic as the client."""

import logging
import ssl

import pytest
from aioquic.h3.connection import H3_ALPN, H3Connection
from aioquic.h3.events import DataReceived
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.connection import QuicConnection

from tests.capture import samples
from tools.capture.certificate import generate_certificate
from tools.capture.page import respond
from tools.capture.quic_server import LiveQuicConnection, QuicDispatcher

_SERVER = ("127.0.0.1", 8445)
_CLIENT = ("127.0.0.1", 50000)
_CERTIFICATE = generate_certificate(samples.HOST)
_MINUTE = 60.0
_ROUND_SECONDS = 0.01
_QUIET_ROUNDS = 5
_MESSAGE = samples.client_hello_message([samples.server_name(), samples.extension(23, bytes(400))])


class Harness:
    """A client and a dispatcher that exchange datagrams directly."""

    def __init__(self) -> None:
        self.now = 1000.0
        self.recorded: list[LiveQuicConnection] = []
        self.dispatcher = QuicDispatcher(_CERTIFICATE, respond, self.recorded.append)
        configuration = QuicConfiguration(
            is_client=True, alpn_protocols=H3_ALPN, server_name=samples.HOST, verify_mode=ssl.CERT_NONE
        )
        self.client = QuicConnection(configuration=configuration)
        self.client.connect(_SERVER, self.now)
        self.http = H3Connection(self.client)
        self.bodies: dict[int, bytes] = {}

    def exchange(self) -> None:
        """Pass datagrams back and forth until nobody has anything to say for a while. aioquic paces what it sends."""
        quiet_rounds = 0
        while quiet_rounds < _QUIET_ROUNDS:
            self.now += _ROUND_SECONDS
            timer = self.client.get_timer()
            if timer is not None and timer <= self.now:
                self.client.handle_timer(self.now)
            outgoing = self.client.datagrams_to_send(self.now)
            incoming = self.dispatcher.handle_timers(self.now)
            for data, _ in outgoing:
                incoming += self.dispatcher.receive(data, _CLIENT, self.now)
            for data, _ in incoming:
                self.client.receive_datagram(data, _SERVER, self.now)
            while (event := self.client.next_event()) is not None:
                for http_event in self.http.handle_event(event):
                    if isinstance(http_event, DataReceived):
                        self.bodies[http_event.stream_id] = self.bodies.get(http_event.stream_id, b"") + http_event.data
            quiet_rounds = 0 if outgoing or incoming else quiet_rounds + 1

    def get(self, target: str) -> bytes:
        stream_id = self.client.get_next_available_stream_id()
        self.http.send_headers(
            stream_id,
            [(b":method", b"GET"), (b":scheme", b"https"), (b":authority", b"a"), (b":path", target.encode())],
            end_stream=True,
        )
        self.exchange()
        return self.bodies[stream_id]


def run_timers_until_quiet(dispatcher: QuicDispatcher, start: float) -> None:
    now = start
    for _ in range(100):
        wakeup = dispatcher.next_wakeup()
        if wakeup is None:
            return
        now = max(now, wakeup)
        dispatcher.handle_timers(now)
    raise AssertionError("the timers did not end")


def test_serves_a_client_and_records_it() -> None:
    harness = Harness()

    assert harness.get("/fetch") == b"fetched\n"

    connection = harness.recorded[0].snapshot()
    assert connection.alpn == "h3"
    assert connection.tls_version == "TLSv1.3"
    assert connection.cipher is not None
    assert connection.ip_version == 4
    assert [request.target for request in connection.requests] == ["/fetch"]


def test_notes_an_ipv6_client() -> None:
    recorded: list[LiveQuicConnection] = []
    dispatcher = QuicDispatcher(_CERTIFICATE, respond, recorded.append)
    packet = samples.initial_packet(samples.crypto_frame(0, _MESSAGE).ljust(1200, b"\x00"))

    dispatcher.receive(packet, ("::1", 50000, 0, 0), 1.0)

    assert recorded[0].ip_version == 6


def test_a_client_can_move_to_another_connection_id() -> None:
    harness = Harness()
    assert harness.get("/fetch") == b"fetched\n"

    harness.client.change_connection_id()

    assert harness.get("/image.png")
    assert len(harness.recorded) == 1
    assert len(harness.recorded[0].snapshot().requests) == 2


def test_a_datagram_for_an_unknown_connection_id_is_dropped() -> None:
    dispatcher = QuicDispatcher(_CERTIFICATE, respond, lambda connection: None)

    assert dispatcher.receive(b"\x40" + bytes(32), _CLIENT, 1.0) == []
    assert dispatcher.next_wakeup() is None


def test_a_short_initial_datagram_opens_nothing() -> None:
    dispatcher = QuicDispatcher(_CERTIFICATE, respond, lambda connection: None)
    packet = samples.initial_packet(samples.crypto_frame(0, _MESSAGE[:100]))
    assert len(packet) < 1200

    assert dispatcher.receive(packet, _CLIENT, 1.0) == []
    assert dispatcher.next_wakeup() is None


def test_an_initial_that_does_not_decrypt_opens_nothing(caplog: pytest.LogCaptureFixture) -> None:
    dispatcher = QuicDispatcher(_CERTIFICATE, respond, lambda connection: None)
    packet = samples.initial_packet(bytes(1200), keys_from=b"other keys")

    with caplog.at_level(logging.WARNING):
        assert dispatcher.receive(packet, _CLIENT, 1.0) == []

    assert dispatcher.next_wakeup() is None
    assert "opened no connection" in caplog.text


def test_a_stray_packet_does_not_cost_a_connection_its_client_hello(caplog: pytest.LogCaptureFixture) -> None:
    recorded: list[LiveQuicConnection] = []
    dispatcher = QuicDispatcher(_CERTIFICATE, respond, recorded.append)
    first, second = samples.initial_datagrams(_MESSAGE)
    dispatcher.receive(first, _CLIENT, 1.0)

    with caplog.at_level(logging.WARNING):
        dispatcher.receive(samples.initial_packet(b"\x01", packet_number=1, keys_from=b"other"), _CLIENT, 1.0)
    dispatcher.receive(second, _CLIENT, 1.0)

    assert "while waiting for a ClientHello" in caplog.text
    assert recorded[0].snapshot().client_hello.message == _MESSAGE


def test_a_connection_goes_on_record_once() -> None:
    recorded: list[LiveQuicConnection] = []
    dispatcher = QuicDispatcher(_CERTIFICATE, respond, recorded.append)
    first, second = samples.initial_datagrams(_MESSAGE)

    dispatcher.receive(first, _CLIENT, 1.0)
    assert recorded == []
    dispatcher.receive(second, _CLIENT, 1.0)
    dispatcher.receive(samples.initial_packet(b"\x01", packet_number=5), _CLIENT, 1.0)

    assert len(recorded) == 1


def test_a_connection_that_never_finishes_its_client_hello_leaves_a_trace(caplog: pytest.LogCaptureFixture) -> None:
    dispatcher = QuicDispatcher(_CERTIFICATE, respond, lambda connection: None)
    first, _ = samples.initial_datagrams(_MESSAGE)
    dispatcher.receive(first, _CLIENT, 1.0)
    assert dispatcher.next_wakeup() is not None

    with caplog.at_level(logging.WARNING):
        run_timers_until_quiet(dispatcher, 1.0)

    assert dispatcher.next_wakeup() is None
    assert "before its ClientHello was complete" in caplog.text


def test_a_clean_close_is_no_error() -> None:
    harness = Harness()
    harness.get("/fetch")

    harness.client.close()
    harness.exchange()
    run_timers_until_quiet(harness.dispatcher, harness.now)

    assert harness.dispatcher.next_wakeup() is None
    assert harness.recorded[0].snapshot().error is None


def test_a_clean_http3_close_is_no_error() -> None:
    harness = Harness()
    harness.get("/fetch")

    harness.client.close(error_code=0x100)
    harness.exchange()
    run_timers_until_quiet(harness.dispatcher, harness.now)

    assert harness.recorded[0].snapshot().error is None


def test_a_close_with_an_error_is_recorded() -> None:
    harness = Harness()
    harness.get("/fetch")

    harness.client.close(error_code=0x123, reason_phrase="tired")
    harness.exchange()
    run_timers_until_quiet(harness.dispatcher, harness.now)

    error = harness.recorded[0].snapshot().error
    assert error is not None
    assert "0x123" in error
    assert "tired" in error


def test_a_client_that_goes_silent_times_out() -> None:
    harness = Harness()
    harness.get("/fetch")

    run_timers_until_quiet(harness.dispatcher, harness.now + 10 * _MINUTE)

    assert harness.dispatcher.next_wakeup() is None
    error = harness.recorded[0].snapshot().error
    assert error is not None
    assert "Idle timeout" in error


def test_after_a_close_datagrams_for_the_connection_are_dropped() -> None:
    harness = Harness()
    harness.get("/fetch")
    harness.client.send_ping(1)
    late = [data for data, _ in harness.client.datagrams_to_send(harness.now)]
    harness.client.close()
    harness.exchange()
    run_timers_until_quiet(harness.dispatcher, harness.now)

    assert late
    for data in late:
        assert harness.dispatcher.receive(data, _CLIENT, harness.now + _MINUTE) == []


def test_http3_errors_close_the_connection_once() -> None:
    harness = Harness()
    harness.get("/fetch")
    bad_stream = harness.client.get_next_available_stream_id()
    harness.client.send_stream_data(bad_stream, b"\x01\x03\xff\xff\xff")
    harness.client.send_stream_data(bad_stream + 4, b"\x01\x03\xff\xff\xff")

    harness.exchange()
    connection = harness.recorded[0].snapshot()

    assert connection.error is not None
    assert connection.error.startswith("ParseError")
    run_timers_until_quiet(harness.dispatcher, harness.now)
    assert harness.recorded[0].snapshot().error == connection.error


def test_a_client_abandoning_a_request_gets_no_answer() -> None:
    harness = Harness()
    harness.get("/fetch")
    stream_id = harness.client.get_next_available_stream_id()
    harness.http.send_headers(stream_id, [(b":method", b"GET"), (b":path", b"/fetch")])
    harness.exchange()

    harness.client.reset_stream(stream_id, 0x10C)
    harness.exchange()

    assert stream_id not in harness.bodies
    assert [request.target for request in harness.recorded[0].snapshot().requests] == ["/fetch", "/fetch"]
