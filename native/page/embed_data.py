#!/usr/bin/env python3
"""Writes a file's bytes as an assembly file that defines a symbol holding them.

The Page program holds ICU's whole data file and V8's startup snapshot this way,
so that it opens no file when it starts. The symbol `<name>` holds the bytes,
padded to whole 32-bit words, and the symbol `<name>_size` holds their number as
a 64-bit integer. Both are hidden from other libraries, as in
../make_icu_data.py, whose assembly this follows. On Windows the assembly goes
into a C++ file instead, as ICU's own build does it there, because the assembler
of Windows reads another syntax.

GN runs this script with the build's own Python. See docs/building.md.
"""

import argparse
import struct
from pathlib import Path

# The data starts at a multiple of this, as ICU's packages do.
ALIGNMENT = 16
WORD = 4


def declarations(symbol: str, size: int, object_format: str) -> list[str]:
    """Returns the lines that make a symbol of `size` bytes global, hidden from other libraries, and start it."""
    if object_format == "mach-o":
        return [f".globl _{symbol}", f".private_extern _{symbol}", f"_{symbol}:"]
    if object_format == "coff":
        return [f".globl {symbol}", f"{symbol}:"]
    return [
        f".globl {symbol}",
        f".hidden {symbol}",
        f".type {symbol}, %object",
        f".size {symbol}, {size}",
        f"{symbol}:",
    ]


def assembly(data: bytes, symbol: str, object_format: str) -> str:
    """Returns the assembly that defines `symbol` with `data` and `symbol`_size with its size.

    For COFF it returns C++ that holds the assembly.
    """
    padded = data + bytes(-len(data) % WORD)
    if object_format == "mach-o":
        lines = [".const"]
    elif object_format == "coff":
        lines = [".section .rdata"]
    else:
        lines = ['.section .note.GNU-stack,"",%progbits', ".section .rodata"]
    lines += [f".p2align {ALIGNMENT.bit_length() - 1}", *declarations(symbol, len(padded), object_format)]
    words = struct.unpack(f"<{len(padded) // WORD}I", padded)
    for start in range(0, len(words), 8):
        lines.append(".long " + ",".join(f"{word:#010x}" for word in words[start : start + 8]))
    lines += [".p2align 3", *declarations(f"{symbol}_size", 8, object_format), f".quad {len(data)}"]
    if object_format == "coff":
        return "__asm__(\n" + "".join(f'    "{line}\\n"\n' for line in lines) + ");\n"
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input", type=Path, help="the file whose bytes the symbol holds")
    parser.add_argument("output", type=Path, help="the assembly file, or the C++ file for COFF, to write")
    parser.add_argument("--symbol", required=True, help="the name of the symbol, as C code declares it")
    parser.add_argument(
        "--format", choices=("elf", "mach-o", "coff"), default="elf", help="the object format of the Host's programs"
    )
    arguments = parser.parse_args()
    arguments.output.write_text(assembly(arguments.input.read_bytes(), arguments.symbol, arguments.format))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
