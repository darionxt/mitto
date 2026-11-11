"""Whether a Page's process is still there, for the tests of Pages through the C API and through Python."""

import ctypes
import os
import sys
import time

__all__ = ["process_is_gone", "wait_until_gone"]

# How long a Page process may outlive what ends it.
_GONE_TIMEOUT_SECONDS = 10


def process_is_gone(process_id: int) -> bool:
    """Return whether the process has ended, or only a handle that another process holds keeps it, on Windows."""
    if sys.platform == "win32":
        synchronize = 0x00100000
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenProcess.restype = ctypes.c_void_p
        handle = kernel32.OpenProcess(synchronize, False, process_id)
        if not handle:
            return True
        try:
            # WAIT_OBJECT_0: it has ended, and only a handle keeps it.
            return kernel32.WaitForSingleObject(ctypes.c_void_p(handle), 0) == 0
        finally:
            kernel32.CloseHandle(ctypes.c_void_p(handle))
    try:
        os.kill(process_id, 0)
    except ProcessLookupError:
        return True
    return False


def wait_until_gone(process_id: int) -> float:
    """Wait for the process to go, and return how long that took."""
    start = time.perf_counter()
    while not process_is_gone(process_id):
        assert time.perf_counter() - start < _GONE_TIMEOUT_SECONDS, f"process {process_id} is still there"
        time.sleep(0.01)
    return time.perf_counter() - start
