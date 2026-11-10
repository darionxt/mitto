"""Tests of the script that makes the source export, none of which need Chromium."""

import base64
import hashlib
import io
import json
import os
import re
import stat
import struct
import subprocess
import sys
import tarfile
import textwrap
import zipfile
from pathlib import Path

import pytest

from tools.workspace import export

needs_file_modes = pytest.mark.skipif(sys.platform == "win32", reason="Windows keeps no file modes")


def write(root: Path, path: str, content: str | bytes = "") -> Path:
    file = root / path
    file.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(content, str):
        file.write_text(content)
    else:
        file.write_bytes(content)
    return file


def test_the_published_export_is_of_the_engine_release() -> None:
    pin = json.loads(export.PIN_FILE.read_text())

    assert f"/download/chromium-{export.read_tag()}/" in pin["url"]
    assert re.fullmatch(r"[0-9a-f]{64}", pin["sha256"])


def test_project_lists_what_gn_read_and_what_targets_declare() -> None:
    project = {
        "build_settings": {"gen_input_files": ["//BUILD.gn", "//build/config/BUILDCONFIG.gn"]},
        "targets": {
            "//net:net": {"sources": ["//net/a.cc", "//out/mitto/gen/b.cc"], "public": ["//net/a.h"]},
            # A target that does not restrict its public headers says so with a string.
            "//base:base": {"sources": ["//base/c.cc"], "public": "*"},
            "//tools:action": {"script": "//tools/d.py", "inputs": ["//tools/e.txt", "/usr/include/f.h"]},
        },
    }

    assert export.paths_in_project(project) == {
        "BUILD.gn",
        "build/config/BUILDCONFIG.gn",
        "net/a.cc",
        "out/mitto/gen/b.cc",
        "net/a.h",
        "base/c.cc",
        "tools/d.py",
        "tools/e.txt",
    }


def test_ninja_deps_gives_the_indented_lines() -> None:
    text = textwrap.dedent("""\
        obj/a.o: #deps 2, deps mtime 123 (VALID)
            ../../a.cc
            gen/a.h

        obj/b.o: #deps 0, deps mtime 123 (STALE)

    """)

    assert export.paths_in_ninja_deps(text) == {"../../a.cc", "gen/a.h"}


def test_depfile_handles_continuation_lines_and_escapes() -> None:
    text = "gen/out.h: ../../a.py \\\n  ../../b\\ c.py\ngen/other.h: ../../d$$.py\nnot a rule\n"

    assert export.paths_in_depfile(text) == {"../../a.py", "../../b c.py", "../../d$.py"}


def test_commands_give_the_paths_in_flags() -> None:
    text = (
        '../../tools/clang -I../../third_party/foo --suppress=../../build/list.txt "../../quoted.py" '
        "-Wl,--script=../../native.lds,--other gen/out.o\n"
    )

    assert export.paths_in_commands(text) == {
        "../../tools/clang",
        "../../third_party/foo",
        "../../build/list.txt",
        "../../quoted.py",
        "../../native.lds",
    }


def test_commands_on_windows_can_name_files_with_backslashes() -> None:
    text = "..\\..\\third_party\\llvm-build\\bin\\clang-cl.exe /c ../../net/a.cc\n"

    assert export.paths_in_commands(text) == {"..\\..\\third_party\\llvm-build\\bin\\clang-cl.exe", "../../net/a.cc"}


def test_commands_give_the_system_include_directories() -> None:
    text = "clang++ -isystem../../third_party/libc++/src/include -isystem ../../other -I../../not_system\n"

    assert export.system_include_dirs(text) == {"../../third_party/libc++/src/include", "../../other"}


def test_commands_on_windows_give_their_system_include_directories() -> None:
    text = '"-imsvc../../../../../Program Files/VC/include" /imsvc../../third_party/win -I../../not_system\n'

    assert export.system_include_dirs(text) == {"../../../../../Program", "../../third_party/win"}


def test_source_relative_leaves_out_what_lies_elsewhere() -> None:
    src = Path("/w/src")
    build_dir = src / "out/x"

    assert export.source_relative("../../net/a.cc", src, build_dir) == "net/a.cc"
    assert export.source_relative("gen/a.h", src, build_dir) == "out/x/gen/a.h"
    assert export.source_relative("/w/src/net/a.cc", src, build_dir) == "net/a.cc"
    assert export.source_relative("/usr/include/a.h", src, build_dir) is None
    assert export.source_relative("/w/src-other/a.h", src, build_dir) is None


@pytest.mark.parametrize(
    "path",
    [
        "net/a.cc",
        "buildtools/third_party/libc++/__config_site",
        "buildtools/third_party/mold/BUILD.gn",
        "build/linux/BUILD.gn",
        "third_party/llvm-libc/src/LICENSE.TXT",
        "build/toolchain/win/rc/win/rc.exe.sha1",
        "build/toolchain/win/rc/rc.py",
    ],
)
def test_source_files_can_be_kept(path: str) -> None:
    assert export.is_kept_path(path)


@pytest.mark.parametrize(
    "path",
    [
        "out/mitto/gen/a.h",
        "mitto/BUILD.gn",
        "third_party/llvm-build/Release+Asserts/bin/clang",
        "third_party/rust-toolchain/bin/rustc",
        "third_party/cpython3/host/bin/python3",
        "buildtools/linux64/gn",
        "buildtools/mac/gn",
        "buildtools/win/gn.exe",
        "build/toolchain/win/rc/win/rc.exe",
        "buildtools/third_party/mold/cipd/mold",
        "build/linux/debian_bullseye_amd64-sysroot/usr/include/stdio.h",
        "build/__pycache__/a.pyc",
        "third_party/foo/.git/HEAD",
    ],
)
def test_output_and_toolchain_cannot_be_kept(path: str) -> None:
    assert not export.is_kept_path(path)


def test_a_path_through_a_symlink_keeps_the_link_and_the_target(tmp_path: Path) -> None:
    write(tmp_path, "real/dir/a.h")
    (tmp_path / "link").symlink_to("real/dir")

    assert export.with_symlinks(tmp_path, "link/a.h") == {"link", "real/dir/a.h"}
    assert export.with_symlinks(tmp_path, "real/dir/a.h") == {"real/dir/a.h"}


def test_a_symlink_out_of_the_tree_stops_the_export(tmp_path: Path) -> None:
    (tmp_path / "src").mkdir()
    (tmp_path / "src/link").symlink_to("..")

    with pytest.raises(export.ExportError, match="leaves the source tree"):
        export.with_symlinks(tmp_path / "src", "link/a.h")


def test_licences_on_the_way_to_a_kept_file_are_kept(tmp_path: Path) -> None:
    write(tmp_path, "LICENSE")
    write(tmp_path, "AUTHORS")
    write(tmp_path, "third_party/foo/COPYING.LESSER")
    write(tmp_path, "third_party/foo/src/a.c")
    write(tmp_path, "third_party/foo/src/LICENSE-MIT")
    write(tmp_path, "third_party/foo/src/other/LICENSE")
    write(tmp_path, "third_party/foo/not_a_licence.txt")
    write(tmp_path, "third_party/bar/LICENSE")

    assert export.licence_files(tmp_path, ["third_party/foo/src/a.c"]) == {
        "LICENSE",
        "AUTHORS",
        "third_party/foo/COPYING.LESSER",
        "third_party/foo/src/LICENSE-MIT",
    }


def test_readme_chromium_names_a_licence_with_an_unusual_name_or_place(tmp_path: Path) -> None:
    write(
        tmp_path,
        "third_party/foo/README.chromium",
        "Name: foo\nLicense File: terms.txt, sub/more.txt, //elsewhere/crate.txt, NOT_SHIPPED\nLicense: MIT\n",
    )
    write(tmp_path, "third_party/foo/a.c")
    write(tmp_path, "third_party/foo/terms.txt")
    write(tmp_path, "third_party/foo/sub/more.txt")
    write(tmp_path, "elsewhere/crate.txt")

    assert export.licence_files(tmp_path, ["third_party/foo/a.c"]) == {
        "third_party/foo/README.chromium",
        "third_party/foo/terms.txt",
        "third_party/foo/sub/more.txt",
        "elsewhere/crate.txt",
    }


def test_a_project_without_a_licence_is_found() -> None:
    files = [
        "LICENSE",
        "net/a.cc",
        "third_party/covered/LICENSE",
        "third_party/covered/a.c",
        "third_party/covered/third_party/inner/b.c",
        "third_party/deep/src/LICENSE.TXT",
        "third_party/deep/a.c",
        "third_party/bare/a.c",
        "third_party/rust/chromium_crates_io/vendor/good/LICENSE-MIT",
        "third_party/rust/chromium_crates_io/vendor/good/src/lib.rs",
        "third_party/rust/chromium_crates_io/vendor/bad/src/lib.rs",
        "third_party/single_file.h",
        "buildtools/third_party/libc++/__config_site",
    ]

    assert export.third_party_without_licence(files) == [
        "third_party/bare",
        "third_party/rust/chromium_crates_io/vendor/bad",
    ]


def test_a_prebuilt_program_stops_the_export(tmp_path: Path) -> None:
    write(tmp_path, "tools/prebuilt", b"\x7fELF and the rest")
    write(tmp_path, "DEPS")

    with pytest.raises(export.ExportError, match="prebuilt program"):
        export.files_to_keep(tmp_path, ["tools/prebuilt"])


def windows_program(signature: bytes = b"PE\0\0") -> bytes:
    header = b"MZ" + b"\0" * 0x3A + struct.pack("<I", 0x80)
    return header + b"\0" * (0x80 - len(header)) + signature + b"rest of the program"


def test_a_windows_program_is_a_program(tmp_path: Path) -> None:
    assert export.is_program(write(tmp_path, "tool.exe", windows_program()))
    assert export.is_program(write(tmp_path, "tool.so", b"\x7fELF and the rest"))


@pytest.mark.parametrize(
    "content",
    [b"MZ is how this text starts, and it is long enough to have a PE offset field", windows_program(b"NE\0\0"), b"MZ"],
)
def test_text_that_starts_like_a_windows_program_is_not_one(tmp_path: Path, content: bytes) -> None:
    assert not export.is_program(write(tmp_path, "notes.txt", content))


def test_files_to_keep_drops_directories_and_missing_files_and_adds_what_the_merge_reads(tmp_path: Path) -> None:
    write(tmp_path, "DEPS")
    write(tmp_path, "LICENSE")
    write(tmp_path, "build/toolchain/win/rc/win/rc.exe.sha1")
    write(tmp_path, "net/a.cc")
    (tmp_path / "net/dir").mkdir()

    assert export.files_to_keep(tmp_path, ["net/a.cc", "net/dir", "net/missing.h", "out/x/gen.h"]) == {
        "DEPS",
        "LICENSE",
        "build/toolchain/win/rc/win/rc.exe.sha1",
        "net/a.cc",
    }


@pytest.mark.parametrize("platform", ["linux", "win"])
def test_the_modules_that_gn_only_checks_follow_the_host(tmp_path: Path, platform: str) -> None:
    write(tmp_path, "out/x/gen/module_platform.txt", platform)

    assert export.paths_gn_only_checks(tmp_path / "out/x") == {f"build/modules/{platform}/BUILD.gn"}
    assert export.paths_gn_only_checks(tmp_path / "out/other") == set()


def test_the_trace_hook_sees_imports_and_reads(tmp_path: Path) -> None:
    site = tmp_path / "site"
    logs = tmp_path / "logs"
    logs.mkdir()
    write(site, "sitecustomize.py", export.TRACE_HOOK)
    src = tmp_path / "src"
    write(src, "module_used_by_script.py", "VALUE = 1\n")
    write(src, "data.txt", "data\n")
    write(src, "unread.txt", "unread\n")
    write(src, "argument.txt", "only stat it\n")
    write(src, "flag_value.txt", "only stat it\n")
    script = write(src, "script.py", "import module_used_by_script\nopen('data.txt').read()\n")
    env = {
        **os.environ,
        "MITTO_EXPORT_TRACE": str(logs),
        "PYTHONPATH": str(site),
        "PYTHONDONTWRITEBYTECODE": "1",
        "PYTHONPYCACHEPREFIX": str(tmp_path / "pycache"),
    }

    command = [sys.executable, script, "argument.txt", "--input=flag_value.txt", "not_a_file.txt"]
    subprocess.run(command, cwd=src, env=env, check=True)

    read = export.paths_read_by_python(logs, src.resolve())
    assert {"module_used_by_script.py", "data.txt", "argument.txt", "flag_value.txt"} <= read
    assert "unread.txt" not in read
    assert "not_a_file.txt" not in read


SAMPLE_DEPS = """\
vars = {
  'host_git': Str('https://example.com'),
  'gn_version': 'git_revision:abc',
  'ninja_package': 'infra/ninja/',
}
deps = {
  'src/third_party/llvm-build/Release+Asserts': {
    'dep_type': 'gcs',
    'bucket': 'clang-bucket',
    'objects': [
      {'object_name': 'Linux_x64/clang-llvmorg-1.tar.xz', 'sha256sum': 'aa', 'size_bytes': 1},
      {'object_name': 'Linux_x64/clang-tidy-llvmorg-1.tar.xz', 'sha256sum': 'bb', 'size_bytes': 1},
      {'object_name': 'Mac_arm64/clang-llvmorg-1.tar.xz', 'sha256sum': 'cc', 'size_bytes': 1},
      {'object_name': 'Mac_arm64/llvmobjdump-llvmorg-1.tar.xz', 'sha256sum': 'c0', 'size_bytes': 1},
      {'object_name': 'Win/clang-llvmorg-1.tar.xz', 'sha256sum': 'a1', 'size_bytes': 1},
      {'object_name': 'Win/clang-tidy-llvmorg-1.tar.xz', 'sha256sum': 'a2', 'size_bytes': 1},
      {'object_name': 'Win/llvmobjdump-llvmorg-1.tar.xz', 'sha256sum': 'a3', 'size_bytes': 1},
    ],
  },
  'src/third_party/rust-toolchain': {
    'dep_type': 'gcs',
    'bucket': 'clang-bucket',
    'objects': [
      {'object_name': 'Linux_x64/rust-toolchain-1.tar.xz', 'sha256sum': 'dd', 'size_bytes': 1},
      {'object_name': 'Mac_arm64/rust-toolchain-1.tar.xz', 'sha256sum': 'ee', 'size_bytes': 1},
      {'object_name': 'Win/rust-toolchain-1.tar.xz', 'sha256sum': 'e1', 'size_bytes': 1},
      {'object_name': 'Win/rust-libclang-1.tar.xz', 'sha256sum': 'e2', 'size_bytes': 1},
    ],
  },
  'src/build/linux/debian_bullseye_amd64-sysroot': {
    'dep_type': 'gcs',
    'bucket': 'sysroot-bucket',
    'objects': [{'object_name': 'ff', 'sha256sum': 'ff', 'size_bytes': 1}],
  },
  'src/buildtools/linux64': {
    'packages': [{'package': 'gn/gn/linux-${{arch}}', 'version': Var('gn_version')}],
  },
  'src/buildtools/mac': {
    'packages': [{'package': 'gn/gn/mac-${{arch}}', 'version': Var('gn_version')}],
  },
  'src/buildtools/win': {
    'packages': [{'package': 'gn/gn/windows-amd64', 'version': Var('gn_version')}],
  },
  'src/buildtools/third_party/mold/cipd': {
    'packages': [{'package': 'mold/mold', 'version': 'instance-id'}],
  },
  'src/third_party/ninja': {
    'packages': [{'package': Var('ninja_package') + '${{platform}}', 'version': 'version:1'}],
  },
  'src/third_party/cpython3/host': {
    'packages': [{'package': 'cpython3/${{platform}}', 'version': 'version:3'}],
  },
  'src/other': Var('host_git') + '/other.git' + '@' + 'abc',
}
hooks = [{'name': 'not read', 'action': ['python3', Unknown('anything')]}]
"""

CIPD_DIGEST = hashlib.sha256(b"package").digest()
CIPD_INSTANCE = base64.urlsafe_b64encode(CIPD_DIGEST + b"\x02").decode().rstrip("=")
RC_SHA1 = hashlib.sha1(b"rc").hexdigest()


def stores(resolved: list[tuple[str, str]] | None = None) -> export.PackageStores:
    """Returns stores that answer from memory, and note each CIPD package asked for."""

    def cipd_instance(package: str, version: str) -> str:
        if resolved is not None:
            resolved.append((package, version))
        return CIPD_INSTANCE

    def gcs_sha256(url: str, sha1: str) -> str:
        assert sha1 == RC_SHA1
        return hashlib.sha256(b"rc").hexdigest()

    return export.PackageStores(cipd_instance, gcs_sha256)


def rc_sources(tmp_path: Path, sha1: str = RC_SHA1) -> dict[str, Path]:
    path = f"{export.RC_EXE}.sha1"
    return {path: write(tmp_path, path, sha1 + "\n")}


def test_deps_evaluates_vars_and_concatenation() -> None:
    deps = export.evaluate_deps(SAMPLE_DEPS)

    assert deps["vars"]["host_git"] == "https://example.com"
    assert deps["deps"]["src/other"] == "https://example.com/other.git@abc"
    assert deps["deps"]["src/buildtools/linux64"]["packages"][0]["version"] == "git_revision:abc"


def test_deps_refuses_what_it_cannot_read() -> None:
    with pytest.raises(export.ExportError, match="cannot read"):
        export.evaluate_deps("vars = {'a': os.system('x')}\n")


def test_cipd_instance_gives_its_hash() -> None:
    assert export.sha256_of_cipd_instance(CIPD_INSTANCE) == CIPD_DIGEST.hex()


def test_cipd_instance_with_another_hash_is_refused() -> None:
    other = base64.urlsafe_b64encode(CIPD_DIGEST + b"\x01").decode().rstrip("=")

    with pytest.raises(export.ExportError, match="SHA-256"):
        export.sha256_of_cipd_instance(other)


def test_pins_follow_the_host(tmp_path: Path) -> None:
    resolved: list[tuple[str, str]] = []
    deps = export.evaluate_deps(SAMPLE_DEPS)
    sources = rc_sources(tmp_path)
    linux = export.toolchain_pins(deps, sources, export.LINUX, stores(resolved))
    mac = export.toolchain_pins(deps, sources, export.MAC, stores(resolved))
    windows = export.toolchain_pins(deps, sources, export.WINDOWS, stores(resolved))

    assert {pin["path"] for pin in mac} == {
        "third_party/llvm-build/Release+Asserts",
        "third_party/rust-toolchain",
        "buildtools/mac",
        "third_party/ninja",
        "third_party/cpython3/host",
    }
    assert {pin["path"] for pin in linux} == {pin["path"] for pin in mac} - {"buildtools/mac"} | {
        "buildtools/linux64",
        "buildtools/third_party/mold/cipd",
        "build/linux/debian_bullseye_amd64-sysroot",
    }
    clang = linux[0]
    assert clang == {
        "path": "third_party/llvm-build/Release+Asserts",
        "url": "https://storage.googleapis.com/clang-bucket/Linux_x64/clang-llvmorg-1.tar.xz",
        "sha256": "aa",
        "format": "tar",
    }
    assert ("gn/gn/linux-amd64", "git_revision:abc") in resolved
    assert ("gn/gn/mac-arm64", "git_revision:abc") in resolved
    assert ("infra/ninja/mac-arm64", "version:1") in resolved
    gn = next(pin for pin in mac if pin["path"] == "buildtools/mac")
    assert gn == {
        "path": "buildtools/mac",
        "url": f"https://chrome-infra-packages.appspot.com/dl/gn/gn/mac-arm64/+/{CIPD_INSTANCE}",
        "sha256": CIPD_DIGEST.hex(),
        "format": "zip",
    }
    assert {pin["path"]: pin["sha256"] for pin in windows if pin["format"] == "tar"} == {
        "third_party/llvm-build/Release+Asserts": "a1",
        "third_party/rust-toolchain": "e1",
    }
    assert ("gn/gn/windows-amd64", "git_revision:abc") in resolved
    assert ("infra/ninja/windows-amd64", "version:1") in resolved
    assert ("cpython3/windows-amd64", "version:3") in resolved
    assert windows[-1] == {
        "path": "build/toolchain/win/rc/win/rc.exe",
        "url": f"https://storage.googleapis.com/chromium-browser-clang/rc/{RC_SHA1}",
        "sha256": hashlib.sha256(b"rc").hexdigest(),
        "format": "file",
    }


def test_a_missing_or_broken_sha1_file_stops_the_pins(tmp_path: Path) -> None:
    deps = export.evaluate_deps(SAMPLE_DEPS)

    with pytest.raises(export.ExportError, match=r"no build/toolchain/win/rc/win/rc\.exe\.sha1"):
        export.toolchain_pins(deps, {}, export.WINDOWS, stores())
    with pytest.raises(export.ExportError, match="not a SHA-1"):
        export.toolchain_pins(deps, rc_sources(tmp_path, "version:1"), export.WINDOWS, stores())


def test_a_download_with_another_sha1_is_refused(tmp_path: Path) -> None:
    source = write(tmp_path, "rc.exe", "rc")

    assert export.sha256_of_download(source.as_uri(), RC_SHA1) == hashlib.sha256(b"rc").hexdigest()
    with pytest.raises(export.ExportError, match="has SHA-1"):
        export.sha256_of_download(source.as_uri(), "0" * 40)


def test_a_missing_package_stops_the_pins() -> None:
    deps = export.evaluate_deps(SAMPLE_DEPS)
    del deps["deps"]["src/third_party/ninja"]

    with pytest.raises(export.ExportError, match="src/third_party/ninja"):
        export.toolchain_pins(deps, {}, export.LINUX, stores())


def test_an_ambiguous_object_stops_the_pins() -> None:
    deps = export.evaluate_deps(SAMPLE_DEPS)
    objects = deps["deps"]["src/third_party/rust-toolchain"]["objects"]
    objects.append({"object_name": "Linux_x64/rust-toolchain-2.tar.xz", "sha256sum": "00"})

    with pytest.raises(export.ExportError, match="2 objects"):
        export.toolchain_pins(deps, {}, export.LINUX, stores())


def test_merge_is_the_union_of_the_slices(tmp_path: Path) -> None:
    first = tmp_path / "linux"
    second = tmp_path / "mac"
    write(first, "src/shared.h", "same")
    write(first, "src/base/linux.cc")
    write(second, "src/shared.h", "same")
    write(second, "src/base/mac.mm")
    (second / "src/link").symlink_to("base")

    merged = export.merge_slices([first, second])

    assert set(merged) == {"shared.h", "base/linux.cc", "base/mac.mm", "link"}
    assert merged["base/mac.mm"] == second / "src/base/mac.mm"


def test_merge_stops_when_the_same_path_differs(tmp_path: Path) -> None:
    write(tmp_path, "linux/src/shared.h", "one")
    write(tmp_path, "mac/src/shared.h", "two")

    with pytest.raises(export.ExportError, match=r"shared\.h differs"):
        export.merge_slices([tmp_path / "linux", tmp_path / "mac"])


def test_merge_stops_when_a_file_and_a_link_differ(tmp_path: Path) -> None:
    write(tmp_path, "linux/src/shared.h", "one")
    write(tmp_path, "mac/src/other.h", "one")
    (tmp_path / "mac/src/shared.h").symlink_to("other.h")

    with pytest.raises(export.ExportError, match=r"shared\.h differs"):
        export.merge_slices([tmp_path / "linux", tmp_path / "mac"])


def test_merge_takes_the_first_copy_of_a_per_host_file(tmp_path: Path) -> None:
    write(tmp_path, "linux/src/build/config/gclient_args.gni", "checkout_openxr = true\n")
    write(tmp_path, "mac/src/build/config/gclient_args.gni", "checkout_openxr = false\n")

    merged = export.merge_slices([tmp_path / "linux", tmp_path / "mac"])

    assert merged["build/config/gclient_args.gni"].read_text() == "checkout_openxr = true\n"


@needs_file_modes
def test_archive_is_reproducible_and_keeps_modes_and_links(tmp_path: Path) -> None:
    tool = write(tmp_path, "slice/tool.py", "#!/usr/bin/env python3\n")
    tool.chmod(0o755)
    header = write(tmp_path, "slice/dir/a.h", "header\n")
    header.chmod(0o640)
    link = tmp_path / "slice/link.h"
    link.symlink_to("dir/a.h")
    files = {"dir/a.h": header, "tool.py": tool, "link.h": link}
    first = tmp_path / "first.tar.xz"
    second = tmp_path / "second.tar.xz"

    extras = {"export.json": b"{}\n", "patches/a.patch": b"patch\n"}
    export.write_archive(first, "chromium-1", files, extras, 1000)
    os.utime(header, (5000, 5000))
    export.write_archive(second, "chromium-1", files, extras, 1000)

    assert first.read_bytes() == second.read_bytes()
    with tarfile.open(first) as tar:
        members = {member.name: member for member in tar.getmembers()}
        assert list(members) == [
            "chromium-1/export.json",
            "chromium-1/patches/a.patch",
            "chromium-1/src/dir/a.h",
            "chromium-1/src/link.h",
            "chromium-1/src/tool.py",
        ]
        assert stat.S_IMODE(members["chromium-1/src/tool.py"].mode) == 0o755
        assert stat.S_IMODE(members["chromium-1/src/dir/a.h"].mode) == 0o644
        assert members["chromium-1/src/link.h"].issym()
        assert members["chromium-1/src/link.h"].linkname == "dir/a.h"
        assert {member.mtime for member in members.values()} == {1000}
        assert {(member.uid, member.gid, member.uname, member.gname) for member in members.values()} == {(0, 0, "", "")}
        metadata = tar.extractfile("chromium-1/export.json")
        assert metadata is not None
        assert metadata.read() == b"{}\n"
    assert not (tmp_path / "first.tar.xz.partial").exists()


@needs_file_modes
def test_a_file_from_a_slice_without_modes_is_not_executable(tmp_path: Path) -> None:
    linux_tool = write(tmp_path, "linux/src/tool.py")
    windows_tool = write(tmp_path, "win/src/build/toolchain/win/tool_wrapper.py")
    for tool in (linux_tool, windows_tool):
        tool.chmod(0o755)
    archive = tmp_path / "export.tar.xz"
    files = {"tool.py": linux_tool, "build/toolchain/win/tool_wrapper.py": windows_tool}

    export.write_archive(archive, "chromium-1", files, {}, 1000, export.executable_in_merge([tmp_path / "win"]))

    with tarfile.open(archive) as tar:
        assert stat.S_IMODE(tar.getmember("chromium-1/src/tool.py").mode) == 0o755
        assert stat.S_IMODE(tar.getmember("chromium-1/src/build/toolchain/win/tool_wrapper.py").mode) == 0o644


def zip_entry(name: str, mode: int) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name)
    info.external_attr = mode << 16
    return info


@needs_file_modes
def test_a_cipd_package_keeps_modes_and_links(tmp_path: Path) -> None:
    packed = io.BytesIO()
    with zipfile.ZipFile(packed, "w") as archive:
        archive.writestr(zip_entry("bin/python3.11", stat.S_IFREG | 0o755), "program")
        archive.writestr(zip_entry("bin/python3", stat.S_IFLNK | 0o777), "python3.11")
        archive.writestr(zip_entry("lib/data.txt", stat.S_IFREG | 0o644), "data")
        archive.writestr(zip_entry(".cipdpkg/manifest.json", stat.S_IFREG | 0o644), "{}")
    archive_file = write(tmp_path, "package.zip", packed.getvalue())
    destination = tmp_path / "out"

    export.extract_zip(archive_file, destination)

    assert (destination / "bin/python3").is_symlink()
    assert (destination / "bin/python3").readlink() == Path("python3.11")
    assert stat.S_IMODE((destination / "bin/python3.11").stat().st_mode) == 0o755
    assert stat.S_IMODE((destination / "lib/data.txt").stat().st_mode) == 0o644
    assert not (destination / ".cipdpkg").exists()


def test_a_cipd_package_has_a_fixed_time(tmp_path: Path) -> None:
    archive_file = tmp_path / "package.zip"
    with zipfile.ZipFile(archive_file, "w") as packed:
        packed.writestr("bin/tool", "tool")
    destination = tmp_path / "unpacked"

    export.extract_zip(archive_file, destination)

    assert (destination / "bin/tool").stat().st_mtime == export.CIPD_FILE_TIME


def test_download_checks_the_hash(tmp_path: Path) -> None:
    source = write(tmp_path, "source/package", "package")
    digest = hashlib.sha256(b"package").hexdigest()
    downloads = tmp_path / "downloads"

    file = export.download(source.as_uri(), digest, downloads)

    assert file.read_text() == "package"
    # It is there now, so a rerun does not need the source.
    source.unlink()
    assert export.download(source.as_uri(), digest, downloads) == file


def test_download_with_the_wrong_hash_leaves_nothing_behind(tmp_path: Path) -> None:
    source = write(tmp_path, "source/package", "package")
    downloads = tmp_path / "downloads"
    wrong = "0" * 64

    with pytest.raises(export.ExportError, match="has SHA-256"):
        export.download(source.as_uri(), wrong, downloads)

    assert list(downloads.iterdir()) == []


def mach_o(text: bytes, build_path: bytes, identity: bytes, zero_fill_size: int = 16) -> bytes:
    """Returns a small Mach-O file with a code section, an empty data section and a symbol table."""
    header_size = 32
    commands_size = 72 + 2 * 80 + len(identity)
    text_offset = header_size + commands_size

    def section(name: bytes, size: int, offset: int, flags: int) -> bytes:
        names = name.ljust(16, b"\0") + b"__TEXT".ljust(16, b"\0")
        return names + struct.pack("<QQIIIIIIII", 0x1000, size, offset, 0, 0, 0, flags, 0, 0, 0)

    segment = struct.pack("<II16sQQQQIIII", 0x19, 72 + 2 * 80, b"__TEXT", 0, 0, 0, 0, 7, 5, 2, 0)
    sections = section(b"__text", len(text), text_offset, 0) + section(b"__bss", zero_fill_size, 0, 0x1)
    other_command = struct.pack("<II", 0x1B, len(identity)) + identity[8:]
    header = export.MACH_O_MAGIC + struct.pack("<IIIIIII", 0, 0, 6, 2, commands_size, 0, 0)
    return header + segment + sections + other_command + text + build_path


def test_a_mach_o_fingerprint_ignores_where_it_was_built(tmp_path: Path) -> None:
    first = write(tmp_path, "first", mach_o(b"code", b"/first/out/a.o", b"identity" + b"1" * 8))
    second = write(tmp_path, "second", mach_o(b"code", b"/a/longer/second/out/a.o", b"identity" + b"2" * 8))

    assert first.read_bytes() != second.read_bytes()
    assert export.fingerprint_of(first) == export.fingerprint_of(second)


@pytest.mark.parametrize(
    "other",
    [
        mach_o(b"CODE", b"/out/a.o", b"identity" + b"1" * 8),
        mach_o(b"code", b"/out/a.o", b"identity" + b"1" * 8, zero_fill_size=32),
    ],
)
def test_a_mach_o_fingerprint_follows_its_sections(tmp_path: Path, other: bytes) -> None:
    first = write(tmp_path, "first", mach_o(b"code", b"/out/a.o", b"identity" + b"1" * 8))
    second = write(tmp_path, "second", other)

    assert export.fingerprint_of(first) != export.fingerprint_of(second)


def windows_dll(code: bytes, pdb_signature: bytes) -> bytes:
    """Returns a small PE32+ DLL whose one section holds a debug directory, its CodeView record and code."""
    section_offset, section_address = 0x200, 0x1000
    record = b"RSDS" + pdb_signature + struct.pack("<I", 1) + b"mitto_core.dll.pdb\0"
    entry = struct.pack("<IIHHIIII", 0, 0, 0, 0, 2, len(record), section_address + 28, section_offset + 28)
    content = entry + record + code
    directories = [(0, 0)] * 16
    directories[6] = (section_address, len(entry))
    optional = struct.pack("<H", 0x20B).ljust(112, b"\0") + b"".join(struct.pack("<II", *d) for d in directories)
    coff = struct.pack("<HHIIIHH", 0x8664, 1, 0, 0, 0, len(optional), 0x2022)
    section = struct.pack("<8sIIII16x", b".rdata", len(content), section_address, len(content), section_offset)
    headers = b"MZ".ljust(0x3C, b"\0") + struct.pack("<I", 0x40) + b"PE\0\0" + coff + optional + section
    return headers.ljust(section_offset, b"\0") + content


def test_a_windows_dll_fingerprint_ignores_the_signature_of_its_pdb(tmp_path: Path) -> None:
    first = write(tmp_path, "first.dll", windows_dll(b"code", b"\x01" * 8 + b"LLD PDB."))
    second = write(tmp_path, "second.dll", windows_dll(b"code", b"\x02" * 8 + b"LLD PDB."))

    assert first.read_bytes() != second.read_bytes()
    assert export.fingerprint_of(first) == export.fingerprint_of(second)
    assert export.fingerprint_of(first) != hashlib.sha256(first.read_bytes()).hexdigest()


def test_a_windows_dll_fingerprint_follows_its_code(tmp_path: Path) -> None:
    first = write(tmp_path, "first.dll", windows_dll(b"code", b"\x01" * 16))
    second = write(tmp_path, "second.dll", windows_dll(b"CODE", b"\x01" * 16))

    assert export.fingerprint_of(first) != export.fingerprint_of(second)


def test_any_other_file_is_fingerprinted_whole(tmp_path: Path) -> None:
    library = write(tmp_path, "libmitto_core.so", b"\x7fELF and the rest")

    assert export.fingerprint_of(library) == hashlib.sha256(b"\x7fELF and the rest").hexdigest()


def tar_of(tmp_path: Path, name: str, files: dict[str, str]) -> Path:
    archive = tmp_path / name
    with tarfile.open(archive, "w:xz") as tar:
        for path, content in files.items():
            info = tarfile.TarInfo(path)
            info.size = len(content.encode())
            tar.addfile(info, io.BytesIO(content.encode()))
    return archive


def test_an_export_unpacks_whole(tmp_path: Path) -> None:
    archive = tar_of(tmp_path, "a.tar.xz", {"chromium-1/export.json": "{}", "chromium-1/src/a.h": "a"})
    root = tmp_path / "build"
    root.mkdir()

    export.unpack_export(archive, root / "chromium-1")

    assert (root / "chromium-1/src/a.h").read_text() == "a"
    assert [path.name for path in root.iterdir()] == ["chromium-1"]


def test_build_output_restored_from_a_cache_stays_with_its_time(tmp_path: Path) -> None:
    archive = tar_of(tmp_path, "a.tar.xz", {"chromium-1/export.json": "{}", "chromium-1/src/a.h": "a"})
    root = tmp_path / "build"
    restored = root / "chromium-1/src/out/mitto/a.o"
    restored.parent.mkdir(parents=True)
    restored.write_text("object")
    os.utime(restored, (1000, 1000))

    export.unpack_export(archive, root / "chromium-1")

    assert (root / "chromium-1/src/a.h").read_text() == "a"
    assert restored.read_text() == "object"
    assert restored.stat().st_mtime == 1000


def test_the_same_export_is_not_unpacked_again(tmp_path: Path) -> None:
    archive = tar_of(tmp_path, "a.tar.xz", {"chromium-1/export.json": "{}", "chromium-1/src/a.h": "a"})
    root = tmp_path / "build"
    root.mkdir()
    export.unpack_export(archive, root / "chromium-1")
    (root / "chromium-1/src/a.h").write_text("changed by the build")

    export.unpack_export(archive, root / "chromium-1")

    assert (root / "chromium-1/src/a.h").read_text() == "changed by the build"


def test_another_export_of_the_same_release_is_refused(tmp_path: Path) -> None:
    first = tar_of(tmp_path, "a.tar.xz", {"chromium-1/export.json": "{}", "chromium-1/src/a.h": "a"})
    second = tar_of(tmp_path, "b.tar.xz", {"chromium-1/export.json": "{}", "chromium-1/src/a.h": "b"})
    root = tmp_path / "build"
    root.mkdir()
    export.unpack_export(first, root / "chromium-1")

    with pytest.raises(export.ExportError, match="another export"):
        export.unpack_export(second, root / "chromium-1")

    assert (root / "chromium-1/src/a.h").read_text() == "a"


@pytest.mark.parametrize(
    "files",
    [
        {"chromium-1/src/a.h": "no metadata"},
        {"chromium-2/export.json": "{}", "chromium-2/src/a.h": "another release"},
    ],
)
def test_an_archive_that_is_not_the_export_of_the_release_leaves_nothing(tmp_path: Path, files: dict[str, str]) -> None:
    archive = tar_of(tmp_path, "a.tar.xz", files)
    root = tmp_path / "build"
    root.mkdir()

    with pytest.raises(export.ExportError, match="not the export"):
        export.unpack_export(archive, root / "chromium-1")

    assert list(root.iterdir()) == []


def toolchain_metadata(tmp_path: Path) -> dict[str, dict[str, list[dict[str, str]]]]:
    package = tar_of(tmp_path, "package.tar.xz", {"bin/tool": "tool"})
    pin = {
        "path": "third_party/tool",
        "url": package.as_uri(),
        "sha256": hashlib.sha256(package.read_bytes()).hexdigest(),
        "format": "tar",
    }
    return {"toolchain": {"linux": [pin]}}


def test_checkouts_that_share_downloads_each_get_the_toolchain(tmp_path: Path) -> None:
    metadata = toolchain_metadata(tmp_path)
    downloads = tmp_path / "downloads"
    first = tmp_path / "chromium-1"
    second = tmp_path / "chromium-2"
    first.mkdir()
    second.mkdir()

    export.fetch_toolchain(metadata, export.LINUX, first, downloads)
    export.fetch_toolchain(metadata, export.LINUX, second, downloads)

    assert (first / "src/third_party/tool/bin/tool").read_text() == "tool"
    assert (second / "src/third_party/tool/bin/tool").read_text() == "tool"


def test_a_single_program_goes_to_its_path(tmp_path: Path) -> None:
    program = write(tmp_path, "rc.exe", "rc")
    pin = {
        "path": export.RC_EXE,
        "url": program.as_uri(),
        "sha256": hashlib.sha256(b"rc").hexdigest(),
        "format": "file",
    }
    checkout = tmp_path / "chromium-1"
    checkout.mkdir()

    export.fetch_toolchain({"toolchain": {"win": [pin]}}, export.WINDOWS, checkout, tmp_path / "downloads")

    placed = checkout / "src" / export.RC_EXE
    assert placed.read_text() == "rc"
    assert placed.stat().st_mtime == export.CIPD_FILE_TIME


def test_a_toolchain_already_in_a_checkout_is_left_alone(tmp_path: Path) -> None:
    metadata = toolchain_metadata(tmp_path)
    downloads = tmp_path / "downloads"
    checkout = tmp_path / "chromium-1"
    checkout.mkdir()
    export.fetch_toolchain(metadata, export.LINUX, checkout, downloads)
    (checkout / "src/third_party/tool/bin/tool").write_text("left alone")
    for file in downloads.iterdir():
        file.unlink()

    export.fetch_toolchain(metadata, export.LINUX, checkout, downloads)

    assert (checkout / "src/third_party/tool/bin/tool").read_text() == "left alone"


def test_a_tree_with_another_file_or_content_is_not_the_same(tmp_path: Path) -> None:
    write(tmp_path, "a/x", "same")
    write(tmp_path, "a/dir/y", "same")
    write(tmp_path, "b/x", "same")
    write(tmp_path, "b/dir/y", "same")
    assert export.same_tree(tmp_path / "a", tmp_path / "b")

    write(tmp_path, "b/dir/y", "diff")
    assert not export.same_tree(tmp_path / "a", tmp_path / "b")

    write(tmp_path, "b/dir/y", "same")
    write(tmp_path, "b/extra", "")
    assert not export.same_tree(tmp_path / "a", tmp_path / "b")


def test_the_environment_of_a_build_has_no_depot_tools(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("PATH", os.pathsep.join(["/w/depot_tools", "/usr/bin", "/w/depot_tools_not"]))
    monkeypatch.setenv("PYTHONPATH", "/somewhere")

    env = export.build_environment()

    assert env["PATH"].split(os.pathsep) == ["/usr/bin", "/w/depot_tools_not"]
    assert "PYTHONPATH" not in env


def test_a_build_on_windows_uses_the_installed_visual_studio(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(sys, "platform", "win32")
    monkeypatch.delenv("DEPOT_TOOLS_WIN_TOOLCHAIN", raising=False)

    assert export.build_environment()["DEPOT_TOOLS_WIN_TOOLCHAIN"] == "0"


def test_windows_on_x64_is_a_host(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(sys, "platform", "win32")
    monkeypatch.setattr(export.platform, "machine", lambda: "AMD64")

    assert export.current_host() is export.WINDOWS


def test_what_chromium_prints_as_gn_gives_its_values() -> None:
    text = textwrap.dedent("""\
        vs_path = "C:\\\\Program Files\\\\Microsoft Visual Studio\\\\18\\\\Community"
        sdk_version = "10.0.28000.0"
        runtime_dirs = "C:\\\\Windows\\\\System32;C:\\\\Windows\\\\SysWOW64;Arm64Unused"
        quoted = "a \\\"b\\\" \\$c \\\\\\\\d"
        not a value
    """)

    assert export.gn_values(text) == {
        "vs_path": r"C:\Program Files\Microsoft Visual Studio\18\Community",
        "sdk_version": "10.0.28000.0",
        "runtime_dirs": r"C:\Windows\System32;C:\Windows\SysWOW64;Arm64Unused",
        "quoted": 'a "b" $c \\\\d',
    }


def test_a_build_config_adds_to_the_args_of_the_plain_build(tmp_path: Path) -> None:
    export.write_args(export.ASAN, tmp_path)

    assert (tmp_path / "args.gn").read_text() == export.ARGS_GN.read_text() + export.ASAN.args
    assert "is_asan = true" in export.ASAN.args


def test_the_plain_build_has_only_the_args_of_the_plain_build(tmp_path: Path) -> None:
    export.write_args(export.PLAIN, tmp_path)

    assert (tmp_path / "args.gn").read_text() == export.ARGS_GN.read_text()


def test_a_build_that_this_host_has_not_traced_is_refused(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(export, "current_host", lambda: export.MAC)

    with pytest.raises(export.ExportError, match="asan"):
        export.build_from_export(tmp_path / "new", None, export.ASAN)

    assert not (tmp_path / "new").exists()


def test_the_runtime_build_adds_its_own_args_file(tmp_path: Path) -> None:
    export.write_args(export.RUNTIME, tmp_path)

    assert (tmp_path / "args.gn").read_text() == export.ARGS_GN.read_text() + export.RUNTIME_ARGS_GN.read_text()
    assert "mitto_enable_runtime = true" in export.RUNTIME_ARGS_GN.read_text()


def test_every_host_traces_the_runtime() -> None:
    assert all(export.RUNTIME in host.configs for host in export.HOSTS)


@pytest.mark.parametrize(
    ("config", "host", "output"),
    [
        (export.PLAIN, export.LINUX, "libmitto_core.so"),
        (export.ASAN, export.LINUX, "libmitto_core.so"),
        (export.RUNTIME, export.LINUX, "mitto_page"),
        (export.RUNTIME, export.MAC, "mitto_page"),
        (export.RUNTIME, export.WINDOWS, "mitto_page.exe"),
        (export.PLAIN, export.WINDOWS, "mitto_core.dll"),
    ],
)
def test_a_slice_fingerprints_the_library_or_the_program_of_each_build(
    config: export.BuildConfig, host: export.Host, output: str
) -> None:
    assert config.output(host) == output


def test_ninja_runs_as_many_jobs_as_asked(tmp_path: Path) -> None:
    ninja = tmp_path / export.MAC.ninja

    assert export.ninja_command(tmp_path, export.MAC, Path("out/x"), 5) == [ninja, "-j", "5", "-C", Path("out/x")]
    assert export.ninja_command(tmp_path, export.MAC, Path("out/x"), None) == [ninja, "-C", Path("out/x")]
