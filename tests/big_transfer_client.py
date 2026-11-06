"""Moves a body of a given size each way through the public API, and prints how much the process's peak memory grew.

A process of its own, because ru_maxrss never goes down: earlier tests in pytest's process would hide any growth here.
tests/test_big_transfers.py runs this, with the server in its own process.
"""

import argparse
import json
import resource
import sys
import zlib
from collections.abc import Iterator

import mitto

# Bytes that differ, so that a part in the wrong place would change the checksum.
_PART = bytes(range(256)) * 256

# Enough for what a Session holds whatever the size of a body: its connection, buffers and flow control windows.
_WARM_UP_SIZE = 32 * 1024 * 1024


def _parts(size: int) -> Iterator[bytes]:
    left = size
    while left > 0:
        part = _PART[: min(left, len(_PART))]
        left -= len(part)
        yield part


def _peak_bytes() -> int:
    # In bytes on macOS, and in KiB on Linux.
    peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    return peak if sys.platform == "darwin" else peak * 1024


def _both_ways(session: mitto.Session, url: str, size: int, *, known_size: bool) -> tuple[int, int, int]:
    """Return the status of the upload, and how many bytes came back and their checksum."""
    headers = {"Content-Length": str(size)} if known_size else None
    upload = session.post(f"{url}upload", data=_parts(size), headers=headers)
    received = 0
    checksum = 0
    with session.stream("GET", f"{url}download?size={size}") as response:
        for chunk in response.iter_bytes():
            received += len(chunk)
            checksum = zlib.crc32(chunk, checksum)
    return upload.status, received, checksum


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("url")
    parser.add_argument("size", type=int)
    parser.add_argument("--known-size", action="store_true")
    parser.add_argument("--trust-anchor")
    args = parser.parse_args()

    anchors = [args.trust_anchor] if args.trust_anchor else []
    with mitto.Session(additional_trust_anchors=anchors) as session:
        _both_ways(session, args.url, _WARM_UP_SIZE, known_size=args.known_size)
        peak_before = _peak_bytes()
        status, received, checksum = _both_ways(session, args.url, args.size, known_size=args.known_size)
        print(
            json.dumps(
                {
                    "status": status,
                    "received": received,
                    "checksum": checksum,
                    "growth": _peak_bytes() - peak_before,
                    "protocol": session.get(f"{args.url}download?size=0").protocol,
                }
            )
        )


if __name__ == "__main__":
    main()
