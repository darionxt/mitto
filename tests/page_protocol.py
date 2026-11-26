"""The version of the messages between a Page and its Session, as native/page_protocol_version.py works it out.

native/BUILD.gn runs that script on the files that define the bytes of a message, and these helpers run it the same way,
with no build, for the tests of the script and of the Page program.
"""

import re
import subprocess
import sys
from pathlib import Path

__all__ = ["NATIVE", "hashed_files", "protocol_version"]

NATIVE = Path(__file__).parent.parent / "native"
_SCRIPT = NATIVE / "page_protocol_version.py"


def hashed_files() -> list[str]:
    """Return the files that the action hashes, as BUILD.gn names them, relative to native/."""
    build = (NATIVE / "BUILD.gn").read_text(encoding="utf-8")
    listed = re.search(r"^_page_message_sources = \[(.*?)^\]", build, re.MULTILINE | re.DOTALL)
    assert listed, "BUILD.gn has no _page_message_sources"
    return re.findall(r'"([^"]+)"', listed[1])


def protocol_version(source_dir: Path, names: list[str], scratch: Path) -> int:
    """Return the version that the script writes for the files of `names` in `source_dir`, writing its header in
    `scratch`."""
    header = scratch / "page_protocol_version.h"
    subprocess.run([sys.executable, _SCRIPT, source_dir, header, *names], check=True)
    written = header.read_text(encoding="utf-8")
    found = re.search(r"^inline constexpr uint32_t kPageProtocolVersion = 0x([0-9a-f]{8});$", written, re.MULTILINE)
    assert found, written
    return int(found[1], 16)
