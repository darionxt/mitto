"""Tests of the script that writes THIRD_PARTY_NOTICES, none of which need Chromium.

They build small source trees and build graphs of their own. The test that a wheel holds the notice of every project
its library links is in test_wheel.py, because it needs a wheel of the real native core.
"""

import tomllib
from pathlib import Path
from typing import Any

import pytest

from tools.workspace import export, notices

REPOSITORY = Path(__file__).parents[1]

LINUX = export.LINUX.name
MAC = export.MAC.name


def write(root: Path, path: str, content: str = "") -> Path:
    file = root / path
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_text(content)
    return file


def target(kind: str, deps: list[str] | None = None, sources: list[str] | None = None) -> dict[str, Any]:
    return {"type": kind, "deps": deps or [], "sources": sources or []}


def entry(
    name: str, hosts: set[str], body: str = "Licence file: //LICENSE\n\ntext\n", directory: str = ""
) -> notices.Entry:
    return notices.Entry(name, directory or f"//third_party/{name}", frozenset(hosts), body)


def test_the_library_links_code_and_groups_but_not_build_tools() -> None:
    graph = {
        notices.ROOT_TARGET: target("shared_library", ["//net:net", "//base:group"]),
        "//net:net": target(
            "static_library", ["//third_party/zlib:zlib", "//net:generate", "//third_party/rust/serde/v1:lib"]
        ),
        "//base:group": target("group", ["//base:base"]),
        "//base:base": target("source_set"),
        "//third_party/zlib:zlib": target("source_set"),
        "//third_party/rust/serde/v1:lib": target("rust_library", ["//third_party/rust/serde_derive/v1:lib"]),
        # A code generator runs during the build, and so does a Rust procedural macro.
        "//net:generate": target("action", ["//net/tools:generator"]),
        "//net/tools:generator": target("executable", ["//third_party/protobuf:protoc_lib"]),
        "//third_party/protobuf:protoc_lib": target("static_library"),
        "//third_party/rust/serde_derive/v1:lib": target("rust_proc_macro", ["//third_party/rust/syn/v2:lib"]),
        "//third_party/rust/syn/v2:lib": target("rust_library"),
    }

    assert notices.linked_targets(graph) == [
        "//base:base",
        "//base:group",
        notices.ROOT_TARGET,
        "//net:net",
        "//third_party/rust/serde/v1:lib",
        "//third_party/zlib:zlib",
    ]


def test_a_graph_without_the_library_is_refused() -> None:
    with pytest.raises(notices.NoticesError, match="no //mitto:mitto_core"):
        notices.linked_targets({"//net:net": target("static_library")})


def test_code_lies_where_the_sources_of_a_target_are() -> None:
    sources = [
        "//third_party/libc++/src/src/string.cpp",
        "//third_party/libc++/src/include/string",
        "//out/mitto/gen/a.cc",
    ]

    assert notices.code_directories(
        "//buildtools/third_party/libc++:libc++", target("source_set", sources=sources)
    ) == {
        "third_party/libc++/src/src",
        "third_party/libc++/src/include",
    }


def test_generated_code_lies_where_its_target_is() -> None:
    sources = ["//out/mitto/gen/net/nqe/network_id_proto.pb.cc"]

    assert notices.code_directories("//net:net_nqe_proto", target("source_set", sources=sources)) == {"net"}


def test_a_rust_crate_lies_where_its_target_and_readme_are() -> None:
    sources = ["//third_party/rust/chromium_crates_io/vendor/serde-v1/src/lib.rs"]

    assert notices.code_directories("//third_party/rust/serde/v1:lib", target("rust_library", sources=sources)) == {
        "third_party/rust/serde/v1"
    }


def test_the_library_s_own_code_is_mitto_s() -> None:
    sources = ["//mitto/mitto_core.cc", "//mitto/include/mitto/mitto.h"]

    assert notices.code_directories(notices.ROOT_TARGET, target("shared_library", sources=sources)) == set()


def test_public_headers_count_and_absolute_paths_do_not() -> None:
    declared = {
        "type": "source_set",
        "public": ["//url/third_party/mozilla/url_parse.h"],
        "sources": ["/usr/include/a.h"],
    }

    assert notices.code_directories("//url:url", declared) == {"url/third_party/mozilla"}


@pytest.fixture
def tree(tmp_path: Path) -> Path:
    """A source tree with a project of each kind that the projects of a directory come from."""
    write(tmp_path, "LICENSE", "Chromium's licence")
    write(tmp_path, "third_party/boringssl/README.chromium", "Name: BoringSSL\nLicense File: src/LICENSE\n")
    write(tmp_path, "third_party/boringssl/src/LICENSE", "BoringSSL's licence")
    write(tmp_path, "third_party/boringssl/src/third_party/fiat/README.chromium", "Name: Fiat\n")
    write(tmp_path, "third_party/boringssl/src/third_party/fiat/LICENSE", "Fiat's licence")
    write(tmp_path, "third_party/protobuf/README.chromium", "Name: Protocol Buffers\n")
    write(tmp_path, "third_party/protobuf/LICENSE", "Protocol Buffers' licence")
    write(tmp_path, "third_party/protobuf/third_party/utf8_range/LICENSE", "utf8_range's licence")
    write(tmp_path, "third_party/perfetto/README.chromium", "Name: Perfetto\n")
    write(tmp_path, "third_party/perfetto/LICENSE", "Perfetto's licence")
    write(tmp_path, "third_party/perfetto/protos/third_party/android/BUILD.gn")
    write(tmp_path, "third_party/rust/serde/v1/README.chromium", "Name: serde\n")
    library = f"{notices.RUST_LIBRARY}"
    write(tmp_path, f"{library}/core/src/lib.rs")
    write(tmp_path, f"{library}/vendor/hashbrown-0.17.1/LICENSE-MIT", "hashbrown's licence")
    write(tmp_path, f"{library}/vendor/hashbrown-0.17.1/src/lib.rs")
    write(tmp_path, f"{library}/compiler-builtins/LICENSE.txt", "compiler-builtins' licence")
    write(tmp_path, f"{library}/compiler-builtins/compiler-builtins/src/lib.rs")
    return tmp_path


@pytest.mark.parametrize(
    ("directory", "project"),
    [
        ("net/http", ""),
        ("third_party/boringssl/src/crypto", "third_party/boringssl"),
        # A project inside another counts on its own.
        ("third_party/boringssl/src/third_party/fiat", "third_party/boringssl/src/third_party/fiat"),
        ("third_party/rust/serde/v1", "third_party/rust/serde/v1"),
        # One without a README.chromium is where its licence is.
        ("third_party/protobuf/third_party/utf8_range", "third_party/protobuf/third_party/utf8_range"),
        # One with neither belongs to the project around it.
        ("third_party/perfetto/protos/third_party/android/connectivity", "third_party/perfetto"),
        ("buildtools/third_party/libc++", ""),
        (f"{notices.RUST_LIBRARY}/core/src", notices.RUST_LIBRARY),
        (f"{notices.RUST_LIBRARY}/vendor/hashbrown-0.17.1/src", f"{notices.RUST_LIBRARY}/vendor/hashbrown-0.17.1"),
        (
            f"{notices.RUST_LIBRARY}/compiler-builtins/compiler-builtins/src",
            f"{notices.RUST_LIBRARY}/compiler-builtins",
        ),
    ],
)
def test_a_directory_belongs_to_the_nearest_project(tree: Path, directory: str, project: str) -> None:
    assert notices.project_of(tree, directory) == project


def test_a_readme_names_each_dependency_and_its_licence_files(tmp_path: Path) -> None:
    readme = """\
Name: zlib
URL: https://zlib.net
License: Zlib
License File: LICENSE, //third_party/zlib/contrib/LICENSE, missing
Shipped: yes
Description: not a field that the notices use
-------------------- DEPENDENCY DIVIDER --------------------
Name: tests only
License File: LICENSE
Shipped: no
-------------------- DEPENDENCY DIVIDER --------------------
Name: old style
License File: NOT_SHIPPED
-------------------- DEPENDENCY DIVIDER --------------------
Name: no licence file named
"""
    write(tmp_path, "third_party/zlib/README.chromium", readme)
    write(tmp_path, "third_party/zlib/LICENSE")
    write(tmp_path, "third_party/zlib/contrib/LICENSE")

    assert notices.dependencies_of(tmp_path, "third_party/zlib") == [
        notices.Dependency(
            "zlib", "https://zlib.net", "Zlib", ("//third_party/zlib/LICENSE", "//third_party/zlib/contrib/LICENSE")
        ),
        notices.Dependency("no licence file named", "", "", ("//third_party/zlib/LICENSE",)),
    ]


def test_copying_is_the_licence_when_a_readme_names_none_that_exists(tmp_path: Path) -> None:
    write(tmp_path, "base/third_party/cityhash/README.chromium", "Name: CityHash\nLicense File: LICENSE\n")
    write(tmp_path, "base/third_party/cityhash/COPYING")

    (dependency,) = notices.dependencies_of(tmp_path, "base/third_party/cityhash")

    assert dependency.licence_files == ("//base/third_party/cityhash/COPYING",)


def test_a_linked_project_without_a_licence_file_stops_the_notices(tmp_path: Path) -> None:
    write(tmp_path, "third_party/zstd/README.chromium", "Name: Zstandard\nLicense File: src/LICENSE\n")

    with pytest.raises(notices.NoticesError, match=r"Zstandard in //third_party/zstd names no licence file"):
        notices.dependencies_of(tmp_path, "third_party/zstd")


def test_a_missing_licence_of_a_special_case_stops_the_notices(tmp_path: Path) -> None:
    with pytest.raises(notices.NoticesError, match="The Rust Standard Library"):
        notices.dependencies_of(tmp_path, notices.RUST_LIBRARY)


def test_shipped_in_chromium_says_more_than_shipped(tmp_path: Path) -> None:
    write(tmp_path, "third_party/a/README.chromium", "Name: a\nShipped: yes\nShipped in Chromium: no\n")

    assert notices.dependencies_of(tmp_path, "third_party/a") == []


def test_a_project_with_neither_a_readme_nor_a_licence_stops_the_notices(tmp_path: Path) -> None:
    (tmp_path / "third_party/a").mkdir(parents=True)

    with pytest.raises(notices.NoticesError, match=r"neither a README\.chromium nor a licence file"):
        notices.dependencies_of(tmp_path, "third_party/a")


def test_a_readme_that_names_a_field_twice_is_refused() -> None:
    with pytest.raises(notices.NoticesError, match="'name' twice"):
        notices.parse_readme("Name: a\nName: b\n")


def test_a_project_without_a_readme_is_named_after_its_directory(tree: Path) -> None:
    assert notices.dependencies_of(tree, "third_party/protobuf/third_party/utf8_range") == [
        notices.Dependency("utf8_range", "", "", ("//third_party/protobuf/third_party/utf8_range/LICENSE",))
    ]


def test_this_host_s_entries_come_from_what_the_library_links(tree: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    graph = {
        notices.ROOT_TARGET: target("shared_library", ["//net:net"], ["//mitto/mitto_core.cc"]),
        "//net:net": target("static_library", ["//third_party/boringssl:boringssl"], ["//net/http/http_cache.cc"]),
        "//third_party/boringssl:boringssl": target("source_set", sources=["//third_party/boringssl/src/ssl/ssl.cc"]),
    }

    def describe(src: Path, build_dir: Path) -> dict[str, Any]:
        assert (src, build_dir) == (tree, tree / "out/mitto")
        return graph

    monkeypatch.setattr(notices, "describe", describe)

    entries = notices.host_entries(tree, tree / "out/mitto")

    host = export.current_host().name
    assert sorted(entries, key=lambda entry: entry.name) == [
        notices.Entry(
            "BoringSSL",
            "//third_party/boringssl",
            frozenset({host}),
            "\nLicence file: //third_party/boringssl/src/LICENSE\n\nBoringSSL's licence\n",
        ),
        notices.Entry(
            "The Chromium Project",
            "//",
            frozenset({host}),
            "URL: https://www.chromium.org\nLicence: BSD-3-Clause\n\nLicence file: //LICENSE\n\nChromium's licence\n",
        ),
    ]


def test_a_licence_text_has_unix_line_ends_and_no_trailing_space(tmp_path: Path) -> None:
    file = tmp_path / "LICENSE"
    file.write_bytes(b"\r\nCopyright \xa9 2026  \r\n\r\nPermission\t\x0c\r\n\n")

    assert notices.licence_text(file) == "Copyright � 2026\n\nPermission"


def test_a_licence_text_with_the_line_between_entries_is_refused(tmp_path: Path) -> None:
    file = write(tmp_path, "LICENSE", f"a\n{notices.SEPARATOR}\nb\n")

    with pytest.raises(notices.NoticesError, match="separates two entries"):
        notices.licence_text(file)


def test_notices_read_back_as_they_were_written() -> None:
    entries = [
        entry("zlib", {LINUX, MAC}),
        entry("Abseil", {MAC}, "URL: https://abseil.io\n\nLicence file: //third_party/abseil-cpp/LICENSE\n\na\n\nb\n"),
        entry("The Chromium Project", {LINUX, MAC}, directory="//"),
    ]

    text = notices.render(entries)

    assert notices.parse(text) == [entries[2], entries[1], entries[0]]
    assert text.startswith(notices.HEADER + "\n" + notices.SEPARATOR + "\nName: The Chromium Project\n")
    assert "\nLinked on: Linux x86_64, macOS arm64\n" in text


def test_text_that_render_did_not_write_is_refused() -> None:
    with pytest.raises(notices.NoticesError, match="header"):
        notices.parse("Some other notices\n")
    with pytest.raises(notices.NoticesError, match="names a Host"):
        notices.parse(f"{notices.HEADER}\n{notices.SEPARATOR}\nName: a\nDirectory: //a\nLinked on: Windows\n")


def test_a_host_s_part_replaces_only_that_host_s_entries() -> None:
    entries = [
        entry("both", {LINUX, MAC}),
        entry("linux only", {LINUX}),
        entry("mac only", {MAC}),
        entry("changed", {LINUX, MAC}),
    ]
    fresh = [
        entry("both", {MAC}),
        entry("new", {MAC}),
        entry("linux only", {MAC}),
        entry("changed", {MAC}, "new text\n"),
    ]

    merged = notices.merge(entries, MAC, fresh)

    assert sorted(merged, key=lambda entry: entry.name) == [
        entry("both", {LINUX, MAC}),
        entry("changed", {LINUX, MAC}, "new text\n"),
        entry("linux only", {LINUX, MAC}),
        entry("new", {MAC}),
    ]


def test_differences_name_each_project_that_a_host_s_notices_get_wrong() -> None:
    entries = [entry("stale", {MAC}), entry("other host", {LINUX}), entry("changed", {MAC}), entry("same", {MAC})]
    fresh = [entry("other host", {MAC}), entry("changed", {MAC}, "new text\n"), entry("same", {MAC})]

    assert notices.differences(entries, MAC, fresh) == [
        "no notice for other host (//third_party/other host), which the library links on macOS arm64",
        "stale (//third_party/stale) is not linked on macOS arm64, but its entry says so",
        "the notice for changed (//third_party/changed) differs from its licence files",
    ]
    assert notices.differences(notices.merge(entries, MAC, fresh), MAC, fresh) == []


def test_a_part_is_for_one_host() -> None:
    assert notices.host_of([entry("a", {LINUX}), entry("b", {LINUX})]) == LINUX
    with pytest.raises(notices.NoticesError, match="one Host"):
        notices.host_of([entry("a", {LINUX}), entry("b", {MAC})])
    with pytest.raises(notices.NoticesError, match="one Host"):
        notices.host_of([entry("a", {LINUX, MAC})])


def test_the_notices_are_as_the_script_writes_them_and_go_into_each_wheel() -> None:
    text = notices.NOTICES_FILE.read_text()
    entries = notices.parse(text)
    project = tomllib.loads((REPOSITORY / "pyproject.toml").read_text())["project"]

    assert notices.render(entries) == text
    assert entries[0].name == "The Chromium Project"
    assert {host for entry in entries for host in entry.hosts} == set(notices.HOST_NAMES)
    assert sorted(project["license-files"]) == sorted(["LICENSE", "MACHINE_TABLE_NOTICE", notices.NOTICES_FILE.name])
