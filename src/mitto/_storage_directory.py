"""A Session's storage directory, where the native core keeps what its Pages store, and the Machine whose it is.

The native core writes the stores there, such as localStorage's SQLite database LocalStorage, and holds the directory
while its Session is open, so that one Session at a time has it, in any process. The directory names the Machine whose
storage it holds in a file of its own, which this module reads and writes, as a file of cookies names its Machine: a
Session that opens on the directory claims that Machine, and one of another Machine is refused. Private until the
Runtime ships, as the Page API is: Session(_storage=...) takes it.
"""

import dataclasses
import json
import os
from pathlib import Path
from typing import Any, cast

from mitto._machine import Machine, machine_from_saved
from mitto._private_file import create_private_file

__all__ = ["MACHINE_FILE", "MACHINE_FILE_VERSION", "read_storage_machine", "storage_directory", "write_storage_machine"]

MACHINE_FILE = "Machine.json"
"""The file in the directory that names its Machine."""

MACHINE_FILE_VERSION = 1
"""The version of that file, which write_storage_machine() writes."""

_KEYS = frozenset({"version", "machine"})


def storage_directory(storage: str | os.PathLike[str] | None) -> Path | None:
    """Return the absolute path of the directory that Session() takes as `_storage`, or None for none.

    The working directory may change before the Session closes, and the native core only takes an absolute path.

    Raises:
        ValueError: If `storage` is an empty string, which would otherwise mean the working directory.
    """
    if storage is None:
        return None
    if storage == "":
        raise ValueError("_storage is an empty string, which names no directory")
    return Path(storage).resolve()


def read_storage_machine(directory: Path) -> Machine | None:
    """Return the Machine that `directory` names, or None if it names none yet, as a new directory does.

    Raises:
        OSError: If its file cannot be read.
        ValueError: If its file does not name a Machine that Machine takes. The message names the file.
    """
    path = directory / MACHINE_FILE
    try:
        contents = path.read_bytes()
    except FileNotFoundError:
        return None
    try:
        held: object = json.loads(contents)
        if not isinstance(held, dict) or cast(dict[str, object], held).keys() != _KEYS:
            raise ValueError("the file holds its version and machine alone")
        held = cast(dict[str, object], held)
        # True is 1 to Python, but not a version.
        if type(held["version"]) is not int or held["version"] != MACHINE_FILE_VERSION:
            raise ValueError(f"the file is of version {MACHINE_FILE_VERSION}")
        saved = held["machine"]
        if not isinstance(saved, dict):
            raise ValueError("the machine is a JSON object")
        return machine_from_saved(cast(dict[str, Any], saved))
    # JSON nested deeper than Python's stack goes names no Machine either.
    except (TypeError, ValueError, RecursionError) as error:
        raise ValueError(f"{path} does not name a Machine that this Session can claim") from error


def write_storage_machine(directory: Path, machine: Machine) -> None:
    """Make `directory` name `machine`, in a file that only its owner can read or write. A crash leaves no half file.

    Raises:
        OSError: If the file cannot be written.
    """
    contents = {"version": MACHINE_FILE_VERSION, "machine": dataclasses.asdict(machine)}
    data = json.dumps(contents, separators=(",", ":"), sort_keys=True).encode()
    descriptor, temporary = create_private_file(directory, prefix=f".{MACHINE_FILE}.", suffix=".tmp")
    try:
        with os.fdopen(descriptor, "wb") as file:
            file.write(data)
            file.flush()
            os.fsync(file.fileno())
        temporary.replace(directory / MACHINE_FILE)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise
