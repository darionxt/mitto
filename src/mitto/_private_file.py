"""A new file that only its owner can use, on every Host.

On macOS and Linux that is a file of mode 0600, which tempfile.mkstemp() makes whatever the umask. Windows has no mode
bits, and a new file there takes the access list that its folder hands down, which may let other users in. So on Windows
the file is created with an access list of its own, protected from what its folder hands down, that lets in its owner
alone: the user that this process runs as. A rename in the same folder keeps it.
"""

import os
import sys
import tempfile
from pathlib import Path

if sys.platform == "win32":
    import ctypes
    import msvcrt
    import secrets
    from ctypes import wintypes

__all__ = ["create_private_file"]


def create_private_file(directory: Path, prefix: str, suffix: str) -> tuple[int, Path]:
    """Create a new file in `directory` that only its owner can use, named as tempfile.mkstemp() names one.

    Returns:
        A descriptor of the file, open to read and write, and its path.

    Raises:
        OSError: If the file cannot be created, such as a FileNotFoundError if `directory` is not there.
    """
    if sys.platform == "win32":
        return _create_on_windows(directory, prefix, suffix)
    descriptor, name = tempfile.mkstemp(dir=directory, prefix=prefix, suffix=suffix)
    return descriptor, Path(name)


if sys.platform == "win32":
    _TOKEN_QUERY = 0x0008
    _TOKEN_USER = 1  # Of TOKEN_INFORMATION_CLASS.
    _SDDL_REVISION_1 = 1
    _GENERIC_READ = 0x80000000
    _GENERIC_WRITE = 0x40000000
    _FILE_SHARE_READ = 0x00000001
    _FILE_SHARE_WRITE = 0x00000002
    _CREATE_NEW = 1
    _FILE_ATTRIBUTE_NORMAL = 0x00000080
    _INVALID_HANDLE_VALUE = wintypes.HANDLE(-1).value

    class _SecurityAttributes(ctypes.Structure):
        """SECURITY_ATTRIBUTES, which hands CreateFileW() the security descriptor of the file it creates."""

        _fields_ = (
            ("nLength", wintypes.DWORD),
            ("lpSecurityDescriptor", wintypes.LPVOID),
            ("bInheritHandle", wintypes.BOOL),
        )

    _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _kernel32.GetCurrentProcess.argtypes = ()
    _kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    _kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
    _kernel32.CloseHandle.restype = wintypes.BOOL
    _kernel32.LocalFree.argtypes = (wintypes.HLOCAL,)
    _kernel32.LocalFree.restype = wintypes.HLOCAL
    _kernel32.CreateFileW.argtypes = (
        wintypes.LPCWSTR,
        wintypes.DWORD,
        wintypes.DWORD,
        ctypes.POINTER(_SecurityAttributes),
        wintypes.DWORD,
        wintypes.DWORD,
        wintypes.HANDLE,
    )
    _kernel32.CreateFileW.restype = wintypes.HANDLE

    _advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)
    _advapi32.OpenProcessToken.argtypes = (wintypes.HANDLE, wintypes.DWORD, ctypes.POINTER(wintypes.HANDLE))
    _advapi32.OpenProcessToken.restype = wintypes.BOOL
    _advapi32.GetTokenInformation.argtypes = (
        wintypes.HANDLE,
        ctypes.c_int,
        wintypes.LPVOID,
        wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD),
    )
    _advapi32.GetTokenInformation.restype = wintypes.BOOL
    _advapi32.ConvertSidToStringSidW.argtypes = (wintypes.LPVOID, ctypes.POINTER(wintypes.LPVOID))
    _advapi32.ConvertSidToStringSidW.restype = wintypes.BOOL
    _advapi32.ConvertStringSecurityDescriptorToSecurityDescriptorW.argtypes = (
        wintypes.LPCWSTR,
        wintypes.DWORD,
        ctypes.POINTER(wintypes.LPVOID),
        ctypes.POINTER(wintypes.ULONG),
    )
    _advapi32.ConvertStringSecurityDescriptorToSecurityDescriptorW.restype = wintypes.BOOL

    def _create_on_windows(directory: Path, prefix: str, suffix: str) -> tuple[int, Path]:
        user = _user_sid()
        # Owned by that user, with an access list that lets that user alone in, protected (P) from what the folder hands
        # down. An administrator can still take the file, as root can on macOS and Linux.
        descriptor = _security_descriptor(f"O:{user}D:P(A;;FA;;;{user})")
        # Random enough that it names no file already there, which CREATE_NEW refuses.
        path = directory / f"{prefix}{secrets.token_hex(8)}{suffix}"
        try:
            attributes = _SecurityAttributes(ctypes.sizeof(_SecurityAttributes), descriptor.value, False)
            # As os.open() opens the file that mkstemp() makes, with a security descriptor of its own.
            handle = _kernel32.CreateFileW(
                str(path),
                _GENERIC_READ | _GENERIC_WRITE,
                _FILE_SHARE_READ | _FILE_SHARE_WRITE,
                ctypes.byref(attributes),
                _CREATE_NEW,
                _FILE_ATTRIBUTE_NORMAL,
                None,
            )
            if handle == _INVALID_HANDLE_VALUE:
                raise _last_error(path)
        finally:
            _kernel32.LocalFree(descriptor)
        try:
            return msvcrt.open_osfhandle(handle, os.O_RDWR | os.O_BINARY | os.O_NOINHERIT), path
        except BaseException:
            _kernel32.CloseHandle(handle)
            path.unlink(missing_ok=True)
            raise

    def _user_sid() -> str:
        """Return the SID of the user that this process runs as, in its string form."""
        token = wintypes.HANDLE()
        if not _advapi32.OpenProcessToken(_kernel32.GetCurrentProcess(), _TOKEN_QUERY, ctypes.byref(token)):
            raise _last_error()
        try:
            size = wintypes.DWORD()
            # This one fails, saying how large the TOKEN_USER is.
            _advapi32.GetTokenInformation(token, _TOKEN_USER, None, 0, ctypes.byref(size))
            token_user = ctypes.create_string_buffer(size.value)
            if not _advapi32.GetTokenInformation(token, _TOKEN_USER, token_user, size, ctypes.byref(size)):
                raise _last_error()
        finally:
            _kernel32.CloseHandle(token)
        # A TOKEN_USER starts with a pointer to the SID, which lies further on in the same buffer.
        sid = wintypes.LPVOID.from_buffer(token_user)
        text = wintypes.LPVOID()
        if not _advapi32.ConvertSidToStringSidW(sid, ctypes.byref(text)):
            raise _last_error()
        try:
            return ctypes.wstring_at(text)
        finally:
            _kernel32.LocalFree(text)

    def _security_descriptor(sddl: str) -> wintypes.LPVOID:
        """Return the security descriptor that `sddl` describes, which the caller frees with LocalFree()."""
        descriptor = wintypes.LPVOID()
        if not _advapi32.ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, _SDDL_REVISION_1, ctypes.byref(descriptor), None
        ):
            raise _last_error()
        return descriptor

    def _last_error(path: Path | None = None) -> OSError:
        """Return the OSError of the last error of a Windows function that this module called, naming `path` if given."""
        code = ctypes.get_last_error()
        return OSError(None, ctypes.FormatError(code), None if path is None else str(path), code)
