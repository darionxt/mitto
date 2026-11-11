#!/usr/bin/env python3
"""Checks that the bindings generator refuses an extended attribute it does not know, with a message that names it.

GN runs this with the build's own Python on a database that Blink's scripts built from the IDL files next to it, and
it writes the stamp file only if the generator refused the database with the expected message.
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import generate_bindings  # noqa: E402, which the line above makes importable.


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--database", type=Path, required=True)
    parser.add_argument("--expect", required=True, help="the message the generator must refuse the database with")
    parser.add_argument("--stamp", type=Path, required=True)
    arguments = parser.parse_args()
    sys.path[:0] = generate_bindings.import_paths(arguments.source_root)

    import web_idl  # pyright: ignore[reportMissingImports]

    from bindings_generator.errors import UnhandledError
    from bindings_generator.web_idl_reader import read_database

    database = web_idl.Database.read_from_file(str(arguments.database))  # pyright: ignore[reportUnknownMemberType, reportUnknownVariableType]
    try:
        read_database(database)
    except UnhandledError as error:
        if str(error) != arguments.expect:
            print(f"generator_test.py: the generator refused it with {error!r}, not {arguments.expect!r}", file=sys.stderr)
            return 1
        arguments.stamp.write_text(f"{error}\n", encoding="utf-8")
        return 0
    print("generator_test.py: the generator read the test database without refusing it", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
