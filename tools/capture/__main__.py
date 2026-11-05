"""Record a client: uv run python -m tools.capture --out DIR."""

import argparse
import json
import sys
from pathlib import Path

from tools.capture.normal_form import RecordingError, dumps_normal_form, normal_form
from tools.capture.page import RequestKind
from tools.capture.recording import HTTP1, HTTP2, HTTP3, raw_form
from tools.capture.server import DEFAULT_HOST, CaptureServer

_FILE_STEMS = {HTTP1: "http1", HTTP2: "http2", HTTP3: "http3"}
_POLL_SECONDS = 0.1


def main() -> int:
    parser = argparse.ArgumentParser(prog="python -m tools.capture", description="Record what a client sends.")
    parser.add_argument("--out", type=Path, required=True, help="directory for the recordings")
    parser.add_argument("--host", default=DEFAULT_HOST, help="a name ending in .localhost (default: %(default)s)")
    parser.add_argument("--http1-port", type=int, default=8443, help="HTTP/1.1 port (default: %(default)s)")
    parser.add_argument("--http2-port", type=int, default=8444, help="HTTP/2 port (default: %(default)s)")
    parser.add_argument("--http3-port", type=int, default=8445, help="HTTP/3 UDP port (default: %(default)s)")
    args = parser.parse_args()

    out: Path = args.out
    out.mkdir(parents=True, exist_ok=True)
    try:
        server = CaptureServer(
            args.host, http1_port=args.http1_port, http2_port=args.http2_port, http3_port=args.http3_port
        )
    except ValueError as error:
        parser.error(str(error))
    try:
        server.start()
    except OSError as error:
        print(
            f"Cannot listen on the ports: {error}. --http1-port, --http2-port and --http3-port choose others.",
            file=sys.stderr,
        )
        return 1
    try:
        return _record(server, out)
    finally:
        server.close()


def _record(server: CaptureServer, out: Path) -> int:
    """Say how to reach the server, and write what each port records. Returns the exit code."""
    print(f"SPKI hash: {server.spki_hash}")
    print(f"HTTP/1.1:  {server.url(HTTP1)}")
    print(f"HTTP/2:    {server.url(HTTP2)}")
    print(f"HTTP/3:    {server.url(HTTP3)}")
    print()
    print("Start each recording from a new, empty user data directory:")
    command = (
        f'  chrome --user-data-dir="NEW_EMPTY_DIRECTORY" --ignore-certificate-errors-spki-list={server.spki_hash}'
        " --no-first-run --no-default-browser-check"
    )
    print(f"{command} URL")
    print()
    print("For HTTP/3, Chrome also has to be told to use QUIC there:")
    print(f"{command} --origin-to-force-quic-on={server.authority(HTTP3)} {server.url(HTTP3)}")
    print()
    print("Waiting for the test page to finish on all three ports. Ctrl-C stops early.", flush=True)
    pending = dict(_FILE_STEMS)
    failed = False
    try:
        while pending:
            for protocol, stem in list(pending.items()):
                if server.wait_for_page(protocol, _POLL_SECONDS):
                    failed = not _write(server, protocol, out, stem) or failed
                    del pending[protocol]
    except KeyboardInterrupt:
        return 130
    return 1 if failed else 0


def _write(server: CaptureServer, protocol: str, out: Path, stem: str) -> bool:
    """Write a port's recordings. Returns whether they make a complete normal form."""
    recording = server.recording(protocol)
    raw_path = out / f"{stem}.raw.json"
    raw_path.write_text(json.dumps(raw_form(recording), indent=2) + "\n", encoding="utf-8", newline="\n")
    try:
        form = normal_form(recording)
    except RecordingError as error:
        print(f"{protocol}: {error}. See {raw_path}.", file=sys.stderr)
        return False
    path = out / f"{stem}.json"
    path.write_text(dumps_normal_form(form), encoding="utf-8", newline="\n")
    requests = form["requests"]
    assert isinstance(requests, dict)
    missing = [kind.value for kind in RequestKind if kind.value not in requests]
    if missing:
        print(f"{protocol}: no request of kind {', '.join(missing)}. See {raw_path}.", file=sys.stderr)
        return False
    print(f"{protocol}: wrote {path}", flush=True)
    return True


if __name__ == "__main__":
    sys.exit(main())
