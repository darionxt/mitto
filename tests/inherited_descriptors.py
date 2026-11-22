"""A child process that holds copies of this process's descriptors of a directory and the files at its top, for the tests
of what may hold a Session's directories on macOS and Linux.

A process that starts another gives the child a copy of each of its descriptors while it starts, close-on-exec ones too
until the child's program has loaded. So a child that another thread starts, such as a Page's, can hold the descriptors
of a Session's directory at the moment the Session closes, as this child does for as long as a test needs.
"""

import contextlib
import subprocess
import sys
from collections.abc import Generator
from pathlib import Path

__all__ = ["child_holding_descriptors_of"]

# What a buffer for fcntl()'s F_GETPATH needs: MAXPATHLEN of macOS.
_PATH_BUFFER_SIZE = 1024


def _path_of(descriptor: int) -> str | None:
    """Return the path that one of this process's descriptors refers to, or None for one that refers to no path."""
    try:
        if sys.platform == "darwin":
            import fcntl

            buffer = fcntl.fcntl(descriptor, fcntl.F_GETPATH, bytes(_PATH_BUFFER_SIZE))
            return buffer.split(b"\0", 1)[0].decode()
        return str(Path(f"/proc/self/fd/{descriptor}").readlink())
    except (OSError, UnicodeDecodeError):
        return None


def _descriptors_of(directory: Path) -> list[int]:
    """Return this process's descriptors of `directory` and of the files at its top."""
    root = directory.resolve()
    listing = Path("/dev/fd" if sys.platform == "darwin" else "/proc/self/fd")
    found: list[int] = []
    for entry in listing.iterdir():
        path = _path_of(int(entry.name))
        if path is not None and root in (Path(path), Path(path).parent):
            found.append(int(entry.name))
    return found


@contextlib.contextmanager
def child_holding_descriptors_of(directory: Path) -> Generator[list[int]]:
    """Start a child process that holds copies of this process's descriptors of `directory` and of the files at its top,
    yield them, and end the child when the block ends."""
    descriptors = _descriptors_of(directory)
    child = subprocess.Popen(
        [sys.executable, "-c", "import sys; sys.stdin.read()"], stdin=subprocess.PIPE, pass_fds=descriptors
    )
    try:
        yield descriptors
    finally:
        child.communicate(timeout=60)
