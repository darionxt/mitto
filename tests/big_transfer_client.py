"""Moves a body of a given size each way through the public API, and prints how much the process's peak memory grew.

A process of its own, because a process's peak memory never goes down: earlier tests in pytest's process would hide any
growth here.
tests/test_big_transfers.py runs this, with the server in its own process.
"""

import argparse
import asyncio
import ctypes
import json
import sys
import zlib
from collections.abc import AsyncIterator, Iterator

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


async def _async_parts(size: int) -> AsyncIterator[bytes]:
    for part in _parts(size):
        yield part


class _ProcessMemoryCounters(ctypes.Structure):
    """Windows' PROCESS_MEMORY_COUNTERS."""

    _fields_ = (
        ("cb", ctypes.c_uint32),
        ("PageFaultCount", ctypes.c_uint32),
        ("PeakWorkingSetSize", ctypes.c_size_t),
        ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t),
        ("PeakPagefileUsage", ctypes.c_size_t),
    )


def _peak_bytes() -> int:
    """Return the most memory that the process has had resident at once."""
    if sys.platform == "win32":
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.GetCurrentProcess.restype = ctypes.c_void_p
        kernel32.K32GetProcessMemoryInfo.argtypes = (
            ctypes.c_void_p,
            ctypes.POINTER(_ProcessMemoryCounters),
            ctypes.c_uint32,
        )
        counters = _ProcessMemoryCounters(cb=ctypes.sizeof(_ProcessMemoryCounters))
        if not kernel32.K32GetProcessMemoryInfo(kernel32.GetCurrentProcess(), ctypes.byref(counters), counters.cb):
            raise ctypes.WinError(ctypes.get_last_error())
        return counters.PeakWorkingSetSize

    import resource

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


async def _both_ways_on_a_loop(
    session: mitto.AsyncSession, url: str, size: int, *, known_size: bool
) -> tuple[int, int, int]:
    """Do what _both_ways() does, through an AsyncSession and an async iterable."""
    headers = {"Content-Length": str(size)} if known_size else None
    upload = await session.post(f"{url}upload", data=_async_parts(size), headers=headers)
    received = 0
    checksum = 0
    async with session.stream("GET", f"{url}download?size={size}") as response:
        async for chunk in response.iter_bytes():
            received += len(chunk)
            checksum = zlib.crc32(chunk, checksum)
    return upload.status, received, checksum


def _outcome(status: int, received: int, checksum: int, peak_before: int, protocol: str) -> str:
    return json.dumps(
        {
            "status": status,
            "received": received,
            "checksum": checksum,
            "growth": _peak_bytes() - peak_before,
            "protocol": protocol,
        }
    )


def _run_blocking(args: argparse.Namespace, anchors: list[str]) -> None:
    with mitto.Session(additional_trust_anchors=anchors) as session:
        _both_ways(session, args.url, _WARM_UP_SIZE, known_size=args.known_size)
        peak_before = _peak_bytes()
        status, received, checksum = _both_ways(session, args.url, args.size, known_size=args.known_size)
        print(
            _outcome(status, received, checksum, peak_before, session.get(f"{args.url}download?size=0").protocol),
        )


async def _run_on_a_loop(args: argparse.Namespace, anchors: list[str]) -> None:
    async with mitto.AsyncSession(additional_trust_anchors=anchors) as session:
        await _both_ways_on_a_loop(session, args.url, _WARM_UP_SIZE, known_size=args.known_size)
        peak_before = _peak_bytes()
        status, received, checksum = await _both_ways_on_a_loop(
            session, args.url, args.size, known_size=args.known_size
        )
        response = await session.get(f"{args.url}download?size=0")
        print(_outcome(status, received, checksum, peak_before, response.protocol))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("url")
    parser.add_argument("size", type=int)
    parser.add_argument("--known-size", action="store_true")
    parser.add_argument("--trust-anchor")
    parser.add_argument("--asyncio", action="store_true")
    args = parser.parse_args()

    anchors = [args.trust_anchor] if args.trust_anchor else []
    if args.asyncio:
        asyncio.run(_run_on_a_loop(args, anchors))
    else:
        _run_blocking(args, anchors)


if __name__ == "__main__":
    main()
