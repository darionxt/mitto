"""The generator's command line, which native/bindings/BUILD.gn runs."""

import argparse
import itertools
import re
import sys
from pathlib import Path
from typing import Any

from bindings_generator.cpp_names import file_name
from bindings_generator.cpp_writer import ImplementedInterface, write_bindings
from bindings_generator.errors import UnhandledError
from bindings_generator.listing import write_listing
from bindings_generator.runtime_features import read_features
from bindings_generator.web_idl_reader import Definitions, read_database

__all__ = ["main"]


def _parse(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", type=Path, required=True, help="Blink's Web IDL database")
    parser.add_argument("--runtime-enabled-features", type=Path, required=True, help="runtime_enabled_features.json5")
    parser.add_argument("--interface", action="append", default=[], help="an interface that has a C++ class")
    parser.add_argument(
        "--header",
        action="append",
        default=[],
        help="INTERFACE=FILE: the class is declared in native/dom/FILE.h, not in the file named after the interface",
    )
    parser.add_argument("--missing", action="append", default=[], help="INTERFACE.MEMBER whose behaviour is missing")
    parser.add_argument(
        "--only-implemented",
        action="append",
        default=[],
        help="an interface whose members are Known Gaps unless --implemented names them",
    )
    parser.add_argument(
        "--implemented", action="append", default=[], help="INTERFACE.MEMBER whose behaviour the class has"
    )
    parser.add_argument(
        "--made-by-known-gaps",
        action="append",
        default=[],
        help="an interface whose class makes an object for a Known Gap with nothing given",
    )
    parser.add_argument("--output-dir", type=Path, required=True, help="where the C++ goes")
    parser.add_argument("--output", action="append", default=[], help="a file that GN expects in the output directory")
    parser.add_argument("--listing", type=Path, required=True, help="where the listing of every interface goes")
    return parser.parse_args(argv)


def _check_feature_names(definitions: Definitions, names: set[str]) -> None:
    """Checks that every feature a construct is behind is in runtime_enabled_features.json5."""
    for interface in definitions.interfaces.values():
        members: list[Any] = [interface, *interface.attributes, *interface.constants, *interface.operation_groups]
        for member in itertools.chain(members, [interface.constructor_group] if interface.constructor_group else []):
            for feature in member.condition.features:
                if feature not in names:
                    raise UnhandledError(f"{interface.name}.{member.name}: {feature} is not a runtime-enabled feature")


def main(argv: list[str], database: Any, runtime_enabled_features: Any) -> int:
    """Reads the database, checks all of it, and writes the listing and the C++ of the implemented interfaces.

    Args:
        argv: the command line, without the program.
        database: the web_idl.Database that --database names, which the caller loads with Blink's scripts.
        runtime_enabled_features: runtime_enabled_features.json5, as the caller's json5 reads it.
    """
    arguments = _parse(argv)
    try:
        definitions = read_database(database)
        features = read_features(runtime_enabled_features["data"])
        _check_feature_names(definitions, {feature.name for feature in features})
        missing = _members_by_interface(arguments.missing)
        listed = _members_by_interface(arguments.implemented)
        only_implemented = set(arguments.only_implemented)
        made_by_known_gaps = set(arguments.made_by_known_gaps)
        headers = _headers(arguments.header)
        implemented: list[ImplementedInterface] = []
        for name in arguments.interface:
            members = listed.pop(name, [])
            made = name in made_by_known_gaps
            header = headers.pop(name, _class_header(name))
            if name in only_implemented:
                if name in missing:
                    raise UnhandledError(f"{name} names both its missing members and its implemented ones")
                implemented.append(
                    ImplementedInterface(name=name, header=header, implemented=tuple(members), made_by_known_gaps=made)
                )
            else:
                implemented.append(
                    ImplementedInterface(
                        name=name,
                        header=header,
                        missing=tuple(missing.pop(name, [])),
                        made_by_known_gaps=made,
                    )
                )
        if made_by_known_gaps - set(arguments.interface):
            unknown = ", ".join(sorted(made_by_known_gaps - set(arguments.interface)))
            raise UnhandledError(f"--made-by-known-gaps names interfaces without a C++ class: {unknown}")
        if missing:
            raise UnhandledError(f"--missing names interfaces without a C++ class: {', '.join(sorted(missing))}")
        if headers:
            raise UnhandledError(f"--header names interfaces without a C++ class: {', '.join(sorted(headers))}")
        if listed or only_implemented - set(arguments.interface):
            unknown = set(listed) | (only_implemented - set(arguments.interface))
            raise UnhandledError(f"--implemented names interfaces without a C++ class: {', '.join(sorted(unknown))}")
        # GN names the files of the tables, as many as it wants of them.
        shards = sum(1 for output in arguments.output if re.fullmatch(r"interfaces_without_class_\d+\.cc", output))
        marks: dict[tuple[str, str, str], str] = {}
        files = write_bindings(definitions, features, implemented, max(shards, 1), marks)
    except UnhandledError as error:
        print(f"generate_bindings.py: {error}", file=sys.stderr)
        return 1
    expected = set(arguments.output)
    if expected != set(files):
        print(
            f"generate_bindings.py: GN expects {sorted(expected - set(files))} and the generator writes "
            f"{sorted(set(files) - expected)} besides; update native/bindings/interfaces.gni",
            file=sys.stderr,
        )
        return 1
    _write_if_changed(arguments.listing, write_listing(list(definitions.interfaces.values()), marks))
    for name, content in files.items():
        _write_if_changed(arguments.output_dir / name, content)
    return 0


def _write_if_changed(path: Path, content: str) -> None:
    """Writes the file only if its content changes, so that ninja, which GN's actions restat, compiles again only what
    a change of the generator or of interfaces.gni changes."""
    if path.is_file() and path.read_text(encoding="utf-8") == content:
        return
    path.write_text(content, encoding="utf-8")


def _members_by_interface(items: list[str]) -> dict[str, list[str]]:
    """Groups INTERFACE.MEMBER items by interface."""
    members: dict[str, list[str]] = {}
    for item in items:
        interface, _, member = item.partition(".")
        members.setdefault(interface, []).append(member)
    return members


def _class_header(name: str) -> str:
    return f"mitto/dom/{file_name(name)}.h"


def _headers(items: list[str]) -> dict[str, str]:
    """The headers that INTERFACE=FILE items name, by interface."""
    headers: dict[str, str] = {}
    for item in items:
        interface, separator, file = item.partition("=")
        if not separator or not file:
            raise UnhandledError(f"--header {item} is not INTERFACE=FILE")
        headers[interface] = f"mitto/dom/{file}.h"
    return headers
