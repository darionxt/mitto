#!/usr/bin/env python3
"""Writes the HTML Standard's table of named character references as C++.

The table comes from Blink's copy of it, html_entity_names.csv, kept here as
Blink has it: one line per name, such as "amp;" or "amp", and the one or two
code points it stands for, such as "U+00026". The C++ holds the same entries,
sorted by name in byte order, which is how the tokenizer narrows them down one
character at a time. Blink's own script packs its table differently, for its
own search.

GN runs this script with the build's own Python.
"""

# Upstream: third_party/blink/renderer/core/html/parser/html_entity_names.csv

import argparse
import csv
import sys
from pathlib import Path
from typing import NamedTuple

HEADER = """\
// Written by make_html_entity_table.py from html_entity_names.csv.

#include "mitto/html/html_entity_table.h"

#include <array>

namespace mitto {

namespace {

"""

FOOTER = """\
}};

}  // namespace

base::span<const HtmlEntity> HtmlEntityTable() {
  return kEntities;
}

}  // namespace mitto
"""


class Entity(NamedTuple):
    name: str
    code_points: tuple[int, ...]


def read_entities(path: Path) -> list[Entity]:
    """Reads the table, and checks what the tokenizer relies on."""
    entities: list[Entity] = []
    with path.open(newline="", encoding="ascii") as file:
        for name, value in csv.reader(file):
            code_points = tuple(int(item.removeprefix("U+"), 16) for item in value.split())
            if not name.rstrip(";").isalnum() or ";" in name.rstrip(";"):
                sys.exit(f"{path}: {name!r} is more than ASCII letters and digits and a semicolon at the end")
            if not 1 <= len(code_points) <= 2:
                sys.exit(f"{path}: {name!r} stands for {len(code_points)} code points")
            entities.append(Entity(name, code_points))
    names = {entity.name for entity in entities}
    if len(names) != len(entities):
        sys.exit(f"{path}: a name is there twice")
    return sorted(entities, key=lambda entity: entity.name.encode("ascii"))


def write_table(entities: list[Entity], path: Path) -> None:
    lines = [HEADER, f"constexpr std::array<HtmlEntity, {len(entities)}> kEntities = {{{{\n"]
    for entity in entities:
        first, second = (*entity.code_points, 0)[:2]
        lines.append(f'    {{"{entity.name}", 0x{first:05X}, 0x{second:04X}}},\n')
    lines.append(FOOTER)
    path.write_text("".join(lines), encoding="ascii")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("table", type=Path, help="html_entity_names.csv")
    parser.add_argument("output", type=Path, help="the C++ file to write")
    arguments = parser.parse_args()
    write_table(read_entities(arguments.table), arguments.output)


if __name__ == "__main__":
    main()
