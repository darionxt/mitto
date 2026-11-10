"""Which files of Chromium's tree the source export holds, from its archive. See docs/building.md.

The archive holds `chromium-<tag>/export.json` and every file under `chromium-<tag>/src/`, as
tools/workspace/export.py writes it.
"""

import hashlib
import io
import json
import lzma
import tarfile
from dataclasses import dataclass
from pathlib import Path

from tools.release.web import Fetch

__all__ = ["PIN_PATH", "ExportError", "SourceExport", "pinned_export", "read_export"]

PIN_PATH = Path(__file__).resolve().parents[1] / "workspace" / "source_export.json"
"""Where the repository pins the export that CI and the release workflow build from."""


class ExportError(Exception):
    """The archive is not the source export, or not the one the pin names."""


@dataclass(frozen=True, slots=True)
class SourceExport:
    """What a source export holds.

    Attributes:
        build: The Chromium build it was made from, such as "157.0.8103.39".
        files: The path of each file it holds, from the top of Chromium's tree, such as "net/http/partial_data.cc".
    """

    build: str
    files: frozenset[str]


def read_export(archive: bytes) -> SourceExport:
    """Return what the archive of a source export holds.

    Raises:
        ExportError: The archive is not a source export.
    """
    try:
        with tarfile.open(fileobj=io.BytesIO(archive), mode="r:xz") as tar:
            names = [member.name for member in tar if member.isfile() or member.issym()]
    except (tarfile.TarError, lzma.LZMAError, EOFError) as error:
        raise ExportError(f"the archive cannot be read: {error}") from error
    roots = {name.split("/", 1)[0] for name in names}
    root = roots.pop() if len(roots) == 1 else ""
    if not root.startswith("chromium-") or f"{root}/export.json" not in names:
        raise ExportError("the archive has no chromium-<build>/export.json, so it is not a source export")
    prefix = f"{root}/src/"
    files = frozenset(name.removeprefix(prefix) for name in names if name.startswith(prefix))
    if not files:
        raise ExportError(f"the archive holds nothing under {prefix}")
    return SourceExport(build=root.removeprefix("chromium-"), files=files)


def pinned_export(fetch: Fetch, pin_path: Path = PIN_PATH) -> SourceExport:
    """Return what the export that the repository pins holds, downloaded from where the pin says.

    Raises:
        ExportError: The download is not the archive that the pin names by its SHA-256, or not a source export.
        WebError: The download failed.
    """
    pin = json.loads(pin_path.read_text(encoding="utf-8"))
    archive = fetch(pin["url"])
    digest = hashlib.sha256(archive).hexdigest()
    if digest != pin["sha256"]:
        raise ExportError(f"{pin['url']} has SHA-256 {digest}, not {pin['sha256']} as {pin_path.name} pins")
    return read_export(archive)
