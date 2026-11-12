"""The file that keeps a Session's cookies with the Machine whose cookies they are.

A file of version 2 holds the Machine beside the cookies. The native core reads only what it lists, the cookies alone in
its own version 1, and refuses what it does not know, so this module splits a file into the Machine and what the native
core reads, and joins what the native core lists with the Machine. A file of version 1 comes from mitto 0.1, which kept no
Machine, since every one of its Sessions claimed the same. A file of version 2 from mitto 0.2 holds a Machine without the
fields that a page reads, which machine_from_saved() draws to fit it.
"""

import dataclasses
import json
import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, cast

from mitto import _native
from mitto._machine import Machine, machine_from_saved
from mitto._private_file import create_private_file

__all__ = ["FILE_VERSION", "CookieFile", "read_cookie_file", "write_cookie_file"]

FILE_VERSION = 2
"""The version of the file that write_cookie_file() writes."""

_MITTO_0_1_VERSION = 1
_MITTO_0_1_MACHINE = "mitto-0.1"

_KEYS = frozenset({"version", "machine", "cookies"})


@dataclass(frozen=True, slots=True)
class CookieFile:
    """What a file of cookies holds, as read_cookie_file() read it.

    Attributes:
        path: Where it was read from, which errors name.
        machine: The Machine whose cookies they are.
        cookies: The cookies, as the JSON that the C API's mitto_session_set_cookies() reads. The native core can still
            refuse them.
    """

    path: str | os.PathLike[str]
    machine: Machine
    cookies: bytes = field(repr=False)

    def add_to(self, native: _native.Session) -> None:
        """Put the cookies in a native Session's store, each replacing any of the same name, domain, path and partition.

        Raises:
            ValueError: If the native core refuses them, in which case none is added. The message names the file and
                leaves out the cookies.
        """
        try:
            native.set_cookies(self.cookies)
        except ValueError as error:
            raise _not_cookies(self.path) from error


def read_cookie_file(path: str | os.PathLike[str]) -> CookieFile:
    """Read a file that write_cookie_file() wrote, or one from mitto 0.1, which holds the Machine of mitto 0.1.

    Raises:
        OSError: If the file cannot be read, such as a FileNotFoundError.
        ValueError: If it is not a file of cookies of a version that this reads, or its Machine is not one that Machine
            takes. The message names the file and leaves out what it holds.
    """
    contents = Path(path).read_bytes()
    try:
        machine, cookies = _split(contents)
    # JSON nested deeper than Python's stack goes is no file of cookies either.
    except (TypeError, ValueError, RecursionError) as error:
        raise _not_cookies(path) from error
    return CookieFile(path, machine, cookies)


def write_cookie_file(path: str | os.PathLike[str], machine: Machine, listed: bytes) -> None:
    """Write the cookies that the native core lists, with their Machine, to a file that only its owner can read or write.

    Raises:
        OSError: If the file cannot be written.
    """
    contents = {
        "version": FILE_VERSION,
        "machine": dataclasses.asdict(machine),
        "cookies": json.loads(listed)["cookies"],
    }
    # As the native core writes its own: keys in order and no spaces.
    _replace_file(Path(path).resolve(), json.dumps(contents, separators=(",", ":"), sort_keys=True).encode())


def _split(contents: bytes) -> tuple[Machine, bytes]:
    """Return the Machine of a file's contents, and its cookies as the native core reads them.

    Raises:
        TypeError: If a value of the Machine is of a type that Machine does not take.
        ValueError: If the contents are not a file of cookies of a version that this reads, or a value of the Machine is
            not one that Machine takes.
    """
    held: object = json.loads(contents)
    if not isinstance(held, dict):
        raise ValueError("a file of cookies holds a JSON object")
    held = cast(dict[str, object], held)
    version = held.get("version")
    # True is 1 to Python, and 1.0 equals it too, but neither is a version.
    if type(version) is not int or version not in (_MITTO_0_1_VERSION, FILE_VERSION):
        raise ValueError(f"a file of cookies is of version {_MITTO_0_1_VERSION} or {FILE_VERSION}")
    if version == _MITTO_0_1_VERSION:
        # What the native core lists, which it reads as it is.
        return Machine.named(_MITTO_0_1_MACHINE), contents
    if held.keys() != _KEYS:
        raise ValueError(f"a file of cookies of version {FILE_VERSION} holds its version, machine and cookies alone")
    cookies = {"version": _native.COOKIES_VERSION, "cookies": held["cookies"]}
    return _machine_of(held["machine"]), json.dumps(cookies).encode()


def _machine_of(saved: object) -> Machine:
    """Return the Machine that a file holds, its seed included.

    A file from mitto 0.2 lacks the fields that it did not know, which are drawn from the Machine's seed to fit the rest,
    so that it is the same Machine in every process.

    Raises:
        TypeError: As Machine does.
        ValueError: If `saved` does not hold each field of a Machine of mitto 0.2 or later and nothing else, or as Machine
            does.
    """
    if not isinstance(saved, dict):
        raise ValueError("the machine of a file of cookies is a JSON object")
    return machine_from_saved(cast(dict[str, Any], saved))


def _not_cookies(path: str | os.PathLike[str]) -> ValueError:
    return ValueError(f"{path} does not hold cookies that this Session can use")


def _replace_file(path: Path, contents: bytes) -> None:
    """Make `path` a file that holds `contents` and that only its owner can use.

    A crash leaves either the old file or the whole new one.
    """
    # The same directory keeps the replace on one file system, a rename that keeps who can use the file.
    descriptor, temporary = create_private_file(path.parent, prefix=f".{path.name}.", suffix=".tmp")
    try:
        with os.fdopen(descriptor, "wb") as file:
            file.write(contents)
            # Otherwise a power cut after the replace could leave an empty file there.
            file.flush()
            os.fsync(file.fileno())
        temporary.replace(path)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise
