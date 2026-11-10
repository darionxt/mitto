"""Tests of reading which files the source export holds, from archives made here."""

import hashlib
import io
import json
import tarfile
from pathlib import Path

import pytest

from tools.release.source_export import ExportError, SourceExport, pinned_export, read_export


def archive(*names: str, links: tuple[str, ...] = ()) -> bytes:
    """Return a tar.xz that holds an empty file of each name, and a symbolic link of each link."""
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w:xz") as tar:
        for name in names:
            tar.addfile(tarfile.TarInfo(name), io.BytesIO(b""))
        for name in links:
            link = tarfile.TarInfo(name)
            link.type, link.linkname = tarfile.SYMTYPE, "elsewhere"
            tar.addfile(link)
    return buffer.getvalue()


def test_the_files_are_those_under_src_with_their_build() -> None:
    export = read_export(
        archive(
            "chromium-157.0.8103.39/README.md",
            "chromium-157.0.8103.39/export.json",
            "chromium-157.0.8103.39/patches/net-chrome-root-store-without-nss.patch",
            "chromium-157.0.8103.39/src/DEPS",
            "chromium-157.0.8103.39/src/net/http/partial_data.cc",
            links=("chromium-157.0.8103.39/src/third_party/llvm-libc/src/shared",),
        )
    )

    assert export == SourceExport(
        build="157.0.8103.39",
        files=frozenset({"DEPS", "net/http/partial_data.cc", "third_party/llvm-libc/src/shared"}),
    )


@pytest.mark.parametrize(
    "body",
    [
        b"not an archive",
        archive("chromium-157.0.8103.39/src/DEPS"),
        archive("mitto-157/export.json", "mitto-157/src/DEPS"),
        archive("chromium-157.0.8103.39/export.json", "chromium-157.0.8103.38/src/DEPS"),
        archive("chromium-157.0.8103.39/export.json"),
    ],
)
def test_an_archive_that_is_not_a_source_export_is_an_error(body: bytes) -> None:
    with pytest.raises(ExportError):
        read_export(body)


def test_the_pinned_export_is_downloaded_from_its_pin_and_checked(tmp_path: Path) -> None:
    body = archive("chromium-157.0.8103.39/export.json", "chromium-157.0.8103.39/src/DEPS")
    pin = tmp_path / "source_export.json"
    pin.write_text(
        json.dumps({"url": "https://example.test/export.tar.xz", "sha256": hashlib.sha256(body).hexdigest()})
    )
    asked: list[str] = []

    def fetch(url: str) -> bytes:
        asked.append(url)
        return body

    assert pinned_export(fetch, pin).files == {"DEPS"}
    assert asked == ["https://example.test/export.tar.xz"]


def test_a_download_that_is_not_the_pinned_archive_is_an_error(tmp_path: Path) -> None:
    pin = tmp_path / "source_export.json"
    pin.write_text(json.dumps({"url": "https://example.test/export.tar.xz", "sha256": "0" * 64}))

    with pytest.raises(ExportError, match="not 0000"):
        pinned_export(lambda _url: archive("chromium-157.0.8103.39/export.json"), pin)
