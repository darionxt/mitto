"""The private ctypes binding of the native core's C API.

Nothing is loaded until load_library() is called, so the package can be imported on a machine without the native
library.
"""

import ctypes
import functools
import os
import sys
from pathlib import Path

__all__ = [
    "ABI_VERSION",
    "LIBRARY_ENV_VAR",
    "LIBRARY_NAME",
    "MittoResponse",
    "NativeLibraryError",
    "find_library",
    "load_library",
]

# The MITTO_ABI_VERSION of the C header this module is written against. Change it together with the declarations below.
ABI_VERSION = 1

# Names the library of a local build. It takes precedence over the library in the installed package.
LIBRARY_ENV_VAR = "MITTO_NATIVE_LIBRARY"

LIBRARY_NAME = "libmitto_core.dylib" if sys.platform == "darwin" else "libmitto_core.so"


class NativeLibraryError(RuntimeError):
    """The native library is missing, cannot be loaded or was built for another ABI version."""


class MittoResponse(ctypes.Structure):
    """A finished response, laid out like the C struct of the same name.

    The native core owns the buffers. Copy what is needed, then release them with mitto_response_free().
    """

    _fields_ = (
        ("status", ctypes.c_int32),
        ("protocol", ctypes.c_char_p),
        ("headers", ctypes.POINTER(ctypes.c_char)),
        ("headers_size", ctypes.c_size_t),
        ("body", ctypes.POINTER(ctypes.c_uint8)),
        ("body_size", ctypes.c_size_t),
    )


def find_library() -> Path | None:
    """Return the path of the native library to load, or None if there is none.

    Raises:
        NativeLibraryError: If LIBRARY_ENV_VAR names something that is not a file. Falling back to another library
            would hide that mistake.
    """
    if override := os.environ.get(LIBRARY_ENV_VAR):
        # An absolute path also stops dlopen() from searching for a bare file name.
        path = Path(override).resolve()
        if not path.is_file():
            raise NativeLibraryError(f"{LIBRARY_ENV_VAR} names {path}, which is not a file")
        return path
    path = Path(__file__).with_name(LIBRARY_NAME)
    return path if path.is_file() else None


@functools.cache
def load_library() -> ctypes.CDLL:
    """Load the native library, check its ABI version and declare the types of its functions.

    The library is loaded once per process. Its functions release the GIL while they run, as all ctypes.CDLL functions
    do, so a blocking call does not stop other Python threads.

    Raises:
        NativeLibraryError: If there is no library, it cannot be loaded or it has another ABI version.
    """
    path = find_library()
    if path is None:
        raise NativeLibraryError(
            f"the installed package has no {LIBRARY_NAME}; set {LIBRARY_ENV_VAR} to the path of a local build"
        )

    try:
        library = ctypes.CDLL(path)

        library.mitto_abi_version.argtypes = ()
        library.mitto_abi_version.restype = ctypes.c_uint32
        # This comes first, because a library with another ABI version may not export what is declared below.
        version: int = library.mitto_abi_version()
        if version != ABI_VERSION:
            raise NativeLibraryError(f"{path} has ABI version {version}, but this package needs {ABI_VERSION}")

        library.mitto_get.argtypes = (ctypes.c_char_p, ctypes.POINTER(MittoResponse))
        library.mitto_get.restype = ctypes.c_int32
        library.mitto_response_free.argtypes = (ctypes.POINTER(MittoResponse),)
        library.mitto_response_free.restype = None
    except (AttributeError, OSError) as error:
        raise NativeLibraryError(f"cannot use {path} as the native library: {error}") from error

    return library
