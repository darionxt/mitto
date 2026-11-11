#!/usr/bin/env python3
"""Writes the Runtime's V8 bindings from Blink's Web IDL database. docs/runtime.md says how.

GN runs this with the build's own Python and the tree's source root, from which it imports Blink's web_idl, which reads
the database, and pyjson5, which reads runtime_enabled_features.json5. The generator itself is the package next to
this file.
"""

import sys
from pathlib import Path

_SOURCE_ROOT_FLAG = "--source-root="


def _import_paths(source_root: Path) -> list[str]:
    return [
        str(Path(__file__).resolve().parent),
        str(source_root / "third_party/blink/renderer/bindings/scripts"),
        str(source_root / "third_party/blink/renderer/build/scripts"),
        str(source_root / "third_party/pyjson5/src"),
        str(source_root / "tools"),
    ]


def run() -> int:
    arguments = sys.argv[1:]
    roots = [argument for argument in arguments if argument.startswith(_SOURCE_ROOT_FLAG)]
    if len(roots) != 1:
        print(f"usage: generate_bindings.py {_SOURCE_ROOT_FLAG}DIR ...", file=sys.stderr)
        return 2
    sys.path[:0] = _import_paths(Path(roots[0].removeprefix(_SOURCE_ROOT_FLAG)))
    arguments.remove(roots[0])

    import json5  # pyright: ignore[reportMissingImports]
    import web_idl  # pyright: ignore[reportMissingImports]

    from bindings_generator import main

    database_path = arguments[arguments.index("--database") + 1]
    features_path = arguments[arguments.index("--runtime-enabled-features") + 1]
    with Path(features_path).open(encoding="utf-8") as file:
        features = json5.load(file)  # pyright: ignore[reportUnknownMemberType, reportUnknownVariableType]
    database = web_idl.Database.read_from_file(database_path)  # pyright: ignore[reportUnknownMemberType, reportUnknownVariableType]
    return main.main(arguments, database, features)


if __name__ == "__main__":
    sys.exit(run())
