#!/usr/bin/env python3
"""Writes the part of ICU's data that the library can read, as an assembly file.

Chromium links all of ICU's data, about 10.8 MB, into the programs that use ICU.
Of the functions that read it, the library links only those below, and each of
them reads items of fixed names. This script copies those items, unchanged,
from Chromium's data file into a data package of ICU's own format, and defines
the symbol through which ICU finds its data, as Chromium's build does for the
whole file. ICU looks an item up by name, so it finds each of these where it
found it in the whole file, and no other is ever asked for.

GN runs this script with the build's own Python. See docs/building.md.
"""

import argparse
import struct
import sys
from pathlib import Path

# Each item, by its name in ICU's data, and the code in the library that reads
# it. A change of what the library links can make ICU read other items, so the
# release routine checks this list again.
ITEMS = (
    # //url's IDNA, which uidna_openUTS46() builds on UTS 46's normalization.
    # The other two are the instances that the same loader in ICU names.
    "uts46.nrm",
    "nfkc.nrm",
    "nfkc_cf.nrm",
    # The names of character sets, which ucnv_open() looks up for the
    # ISO-8859-1 of a Basic or Digest challenge's realm, and the table that this
    # name stands for in Chromium's data: windows-1252, as in HTML.
    "cnvalias.icu",
    "windows-1252-html.cnv",
    # What ICU reads to canonicalize the default locale, when NTLM writes a user
    # name in upper case. keyTypeData points into timezoneTypes.
    "keyTypeData.res",
    "timezoneTypes.res",
    "metadata.res",
    "langInfo.res",
)

# The first bytes of an ICU data file, after the size of its header.
MAGIC = b"\xda\x27"
# What a common data package says it is, and the version of its layout.
PACKAGE_FORMAT = b"CmnD"
PACKAGE_FORMAT_VERSION = 1
# Every item starts at a multiple of this, as in ICU's own packages.
ALIGNMENT = 16
PADDING = b"\xaa"


class DataError(Exception):
    """Stops the build with a message for the person running it."""


def read_package(data: bytes) -> tuple[bytes, dict[str, bytes]]:
    """Returns the header of a common data package, and its items by name.

    An item's bytes run to the start of the next one, so they keep its padding.

    Raises:
        DataError: If the data is not a little-endian common data package.
    """
    (header_size,) = struct.unpack_from("<H", data, 0)
    if data[2:4] != MAGIC:
        raise DataError("the input is not an ICU data file")
    # UDataInfo follows the size and magic: its size, a reserved word, the
    # endianness, the character set, the size of a character and a reserved
    # byte, then the data format and its version.
    is_big_endian = data[8]
    if is_big_endian or data[12:16] != PACKAGE_FORMAT or data[16] != PACKAGE_FORMAT_VERSION:
        raise DataError("the input is not a little-endian ICU common data package")
    (count,) = struct.unpack_from("<I", data, header_size)
    entries = [struct.unpack_from("<II", data, header_size + 4 + 8 * index) for index in range(count)]
    items: dict[str, bytes] = {}
    for index, (name_offset, data_offset) in enumerate(entries):
        name_start = header_size + name_offset
        name = data[name_start : data.index(b"\0", name_start)].decode("ascii")
        end = entries[index + 1][1] if index + 1 < count else len(data) - header_size
        items[name] = data[header_size + data_offset : header_size + end]
    return data[:header_size], items


def write_package(header: bytes, items: dict[str, bytes]) -> bytes:
    """Returns a common data package of the given items, with the given header."""
    names = sorted(items)
    table_size = 4 + 8 * len(names)
    name_offsets: list[int] = []
    name_bytes = bytearray()
    for name in names:
        name_offsets.append(table_size + len(name_bytes))
        name_bytes += name.encode("ascii") + b"\0"
    body = bytearray(table_size) + name_bytes
    data_offsets: list[int] = []
    for name in names:
        body += PADDING * (-(len(header) + len(body)) % ALIGNMENT)
        data_offsets.append(len(body))
        body += items[name]
    struct.pack_into("<I", body, 0, len(names))
    for index, (name_offset, data_offset) in enumerate(zip(name_offsets, data_offsets, strict=True)):
        struct.pack_into("<II", body, 4 + 8 * index, name_offset, data_offset)
    return header + bytes(body)


def symbol_name(items: dict[str, bytes]) -> str:
    """Returns the symbol through which ICU finds its data, such as icudt78_dat.

    Raises:
        DataError: If the items do not name a little-endian ICU version.
    """
    prefixes = {name.partition("/")[0] for name in items}
    if len(prefixes) != 1 or not (prefix := prefixes.pop()).startswith("icudt") or not prefix.endswith("l"):
        raise DataError("the input's items do not name one little-endian ICU version")
    return f"{prefix.removesuffix('l')}_dat"


def assembly(package: bytes, symbol: str, mach_o: bool) -> str:
    """Returns an assembly file that defines a hidden symbol holding the package."""
    if mach_o:
        lines = [
            f".globl _{symbol}",
            f".private_extern _{symbol}",
            ".const",
            f".p2align {ALIGNMENT.bit_length() - 1}",
            f"_{symbol}:",
        ]
    else:
        lines = [
            '.section .note.GNU-stack,"",%progbits',
            ".section .rodata",
            f".balign {ALIGNMENT}",
            f".globl {symbol}",
            f".hidden {symbol}",
            f".type {symbol}, %object",
            f".size {symbol}, {len(package)}",
            f"{symbol}:",
        ]
    words = struct.unpack(f"<{len(package) // 4}I", package)
    for start in range(0, len(words), 8):
        lines.append(".long " + ",".join(f"{word:#010x}" for word in words[start : start + 8]))
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input", type=Path, help="Chromium's ICU data file, icudtl.dat")
    parser.add_argument("output", type=Path, help="the assembly file to write")
    parser.add_argument("--mach-o", action="store_true", help="write for macOS rather than for ELF")
    arguments = parser.parse_args()
    try:
        header, items = read_package(arguments.input.read_bytes())
        symbol = symbol_name(items)
        prefix = symbol.removesuffix("_dat") + "l/"
        missing = [name for name in ITEMS if prefix + name not in items]
        if missing:
            raise DataError(f"ICU's data has no {', '.join(missing)}; see the list in {Path(__file__).name}")
        package = write_package(header, {prefix + name: items[prefix + name] for name in ITEMS})
    except DataError as error:
        print(f"{Path(__file__).name}: error: {error}", file=sys.stderr)
        return 1
    arguments.output.write_text(assembly(package, symbol, arguments.mach_o))
    return 0


if __name__ == "__main__":
    sys.exit(main())
