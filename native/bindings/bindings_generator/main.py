"""The generator's command line, which native/bindings/BUILD.gn runs."""

import argparse
import itertools
import sys
from pathlib import Path
from typing import Any

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
    parser.add_argument("--missing", action="append", default=[], help="INTERFACE.MEMBER whose behaviour is missing")
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
        missing: dict[str, list[str]] = {}
        for item in arguments.missing:
            interface, _, member = item.partition(".")
            missing.setdefault(interface, []).append(member)
        implemented = [
            ImplementedInterface(name=name, header=_class_header(name), missing=tuple(missing.pop(name, [])))
            for name in arguments.interface
        ]
        if missing:
            raise UnhandledError(f"--missing names interfaces without a C++ class: {', '.join(sorted(missing))}")
        files = write_bindings(definitions, features, implemented)
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
    arguments.listing.write_text(write_listing(list(definitions.interfaces.values())), encoding="utf-8")
    for name, content in files.items():
        (arguments.output_dir / name).write_text(content, encoding="utf-8")
    return 0


def _class_header(name: str) -> str:
    from bindings_generator.cpp_names import file_name

    return f"mitto/dom/{file_name(name)}.h"
