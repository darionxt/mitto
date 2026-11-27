#!/usr/bin/env python3
"""Writes the version of the messages that both ends of the Page channel speak.

The version is a fingerprint of the files that define the bytes of a message:
the first 32 bits of the SHA-256 of each file's name and text, in the order of
their names, with LF line endings, so that every Host works out the same number
from the same tree. A Page and a library built from trees whose messages differ
speak different versions, and refuse each other at the hello. No one keeps the
number by hand.

GN runs this script with the build's own Python. See docs/runtime.md.
"""

import argparse
import hashlib
from pathlib import Path

HEADER = """\
// The version of the messages that both ends of the Page channel speak, which
// page_protocol_version.py works out from the files that define their bytes.

#ifndef MITTO_PAGE_PROTOCOL_VERSION_H_
#define MITTO_PAGE_PROTOCOL_VERSION_H_

#include <stdint.h>

namespace mitto {{

inline constexpr uint32_t kPageProtocolVersion = 0x{version:08x};

}}  // namespace mitto

#endif  // MITTO_PAGE_PROTOCOL_VERSION_H_
"""


def fingerprint(source_dir: Path, names: list[str]) -> int:
    """Returns the version of the messages that the files of `names`, relative to `source_dir`, define.

    Each file adds its name, a NUL, its text with LF line endings and another NUL to the hash, so that moving text from
    one file to the next changes the number too."""
    digest = hashlib.sha256()
    for name in sorted(names):
        text = (source_dir / name).read_bytes().replace(b"\r\n", b"\n")
        digest.update(name.encode() + b"\0" + text + b"\0")
    return int.from_bytes(digest.digest()[:4], "big")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source_dir", type=Path, help="the directory that holds the files")
    parser.add_argument("output", type=Path, help="the header to write")
    parser.add_argument("names", nargs="+", help="the files that define the bytes of a message, as BUILD.gn names them")
    args = parser.parse_args()
    source_dir: Path = args.source_dir
    output: Path = args.output
    names: list[str] = args.names
    output.write_text(HEADER.format(version=fingerprint(source_dir, names)), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
