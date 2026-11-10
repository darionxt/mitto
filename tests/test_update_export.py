"""Tests of the script that updates the source export to a later build, with local git repositories only."""

import json
import subprocess
import sys
import tarfile
from pathlib import Path

import pytest

from tools.workspace import export, update_export

OLD = "157.0.8103.27"
NEW = "157.0.8103.39"

needs_posix = pytest.mark.skipif(sys.platform == "win32", reason="the update runs on macOS or Linux")


def test_deps_gives_the_git_repositories_and_their_revisions() -> None:
    deps = {
        "deps": {
            "src/v8": "https://chromium.googlesource.com/v8/v8.git@abc",
            "src/third_party/boringssl/src": {"url": "https://boringssl.googlesource.com/boringssl.git@def"},
            "src/third_party/ninja": {"packages": [{"package": "infra/ninja", "version": "1"}], "dep_type": "cipd"},
            "src/buildtools/linux64": {"url": None, "condition": "host_os == linux"},
            "other/repository": "https://example.com/other.git@123",
        }
    }

    assert update_export.git_dependencies(deps) == {
        "v8": update_export.GitDependency("https://chromium.googlesource.com/v8/v8.git", "abc"),
        "third_party/boringssl/src": update_export.GitDependency(
            "https://boringssl.googlesource.com/boringssl.git", "def"
        ),
    }


def test_a_file_belongs_to_the_innermost_dependency() -> None:
    dependencies = ["third_party/boringssl/src", "third_party/boringssl", "v8"]

    assert (
        update_export.owner_of("third_party/boringssl/src/ssl/ssl_lib.cc", dependencies) == "third_party/boringssl/src"
    )
    assert update_export.owner_of("third_party/boringssl/BUILD.gn", dependencies) == "third_party/boringssl"
    assert update_export.owner_of("v8/include/v8.h", dependencies) == "v8"
    assert update_export.owner_of("v8.gni", dependencies) == ""
    assert update_export.owner_of("net/http/http_cache.cc", dependencies) == ""


@pytest.mark.parametrize(
    ("path", "changed"),
    [
        ("net/BUILD.gn", True),
        ("build/config/compiler/compiler.gni", True),
        ("build/dotfile_settings.gn", True),
        ("third_party/blink/renderer/bindings/scripts/build_web_idl_database.pydeps", True),
        ("net/http/http_cache.cc", False),
        ("v8/src/builtins/array.tq", False),
    ],
)
def test_build_files_are_those_that_can_add_to_the_build(path: str, changed: bool) -> None:
    assert update_export.build_file_changed(path) is changed


def test_includes_and_checks_for_headers_are_found() -> None:
    text = '#include "net/a.h"\n#  include <vector>\n#import "b.h"\n#if __has_include(<sys/x.h>)\n// #include "no.h"\n'

    assert update_export.included_names(text) == {"net/a.h", "vector", "b.h", "sys/x.h"}


def test_a_new_include_of_a_file_the_export_lacks_is_missing() -> None:
    old = b'#include "net/a.h"\n'
    new = b'#include "net/a.h"\n#include "net/b.h"\n#include "base/c.h"\n#include "gen/net/d.h"\n'
    exported = ["net/a.h", "base/c.h"]

    assert update_export.missing_includes("net/x.cc", old, new, exported) == {"net/b.h"}


def test_an_include_found_below_an_include_directory_is_not_missing() -> None:
    new = b"#include <vector>\n#include <openssl/ssl.h>\n"
    exported = ["third_party/libc++/src/include/vector", "third_party/boringssl/src/include/openssl/ssl.h"]

    assert update_export.missing_includes("net/x.h", b"", new, exported) == set()


def test_only_sources_have_includes() -> None:
    assert update_export.missing_includes("net/data.json", b"", b'#include "x.h"\n', []) == set()


def test_new_imports_of_a_script_are_found() -> None:
    old = b"import os\n\nprint(1)\n"
    new = b"import os\nimport json\nfrom pathlib import Path\n\nprint(1)\n"

    assert update_export.added_imports("build/x.py", old, new) == ["import json", "from pathlib import Path"]
    assert update_export.added_imports("build/x.gn", old, new) == []


def test_a_patch_names_the_files_it_changes() -> None:
    patch = "Why.\n\ndiff --git a/net/a.cc b/net/a.cc\n--- a/net/a.cc\n+++ b/net/a.cc\n@@ -1 +1 @@\n-a\n+b\n"

    assert update_export.patched_paths(patch) == {"net/a.cc"}


def test_the_version_of_v8_comes_from_its_header() -> None:
    header = "#define V8_MAJOR_VERSION 15\n#define V8_MINOR_VERSION 7\n#define V8_BUILD_NUMBER 112\n#define V8_PATCH_LEVEL 23\n"

    assert update_export.v8_version(header) == "15.7.112.23"
    with pytest.raises(update_export.UpdateError):
        update_export.v8_version(header.replace("V8_PATCH_LEVEL", "V8_PATCH"))


# A tree with two tags and a dependency, which the update reads as it reads Chromium's.


def git(repository: Path, *arguments: str) -> str:
    return subprocess.run(
        ["git", "-c", "user.name=t", "-c", "user.email=t@example.com", "-C", repository, *arguments],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()


def commit(repository: Path, files: dict[str, str], tag: str | None = None) -> str:
    for path, content in files.items():
        file = repository / path
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(content)
    git(repository, "add", "-A")
    git(repository, "commit", "--quiet", "-m", "change")
    if tag:
        git(repository, "tag", tag)
    return git(repository, "rev-parse", "HEAD")


def new_repository(directory: Path) -> Path:
    directory.mkdir(parents=True)
    git(directory, "init", "--quiet")
    git(directory, "config", "uploadpack.allowAnySHA1InWant", "true")
    return directory


def deps_file(dependency: Path, revision: str) -> str:
    return f"vars = {{'dep_revision': '{revision}'}}\ndeps = {{'src/dep': '{dependency.as_uri()}' + '@' + Var('dep_revision')}}\n"


def old_archive(tmp_path: Path, files: dict[str, str], commit_time: int, patches: dict[str, str]) -> Path:
    tree = tmp_path / "old-export"
    root = tree / f"chromium-{OLD}"
    for path, content in {**files, update_export.LASTCHANGE_COMMIT_TIME: str(commit_time)}.items():
        file = root / "src" / path
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(content)
    metadata = {"chromium_tag": OLD, "patches": patches, "traced_outputs": {"linux": {"mitto": "0"}}, "toolchain": {}}
    (root / "export.json").write_text(json.dumps(metadata))
    archive = tmp_path / "old.tar.xz"
    with tarfile.open(archive, "w:xz") as tar:
        tar.add(root, arcname=root.name)
    return archive


class Tree:
    """chromium/src and one dependency, each at the old build and the new one."""

    def __init__(self, tmp_path: Path, src_change: dict[str, str], dependency_change: dict[str, str]) -> None:
        dependency = new_repository(tmp_path / "upstream-dep")
        self.dependency_files = {"lib.cc": '#include "dep/lib.h"\n', "lib.h": "int f();\n", "BUILD.gn": ""}
        old_revision = commit(dependency, self.dependency_files)
        new_revision = commit(dependency, dependency_change) if dependency_change else old_revision
        src = new_repository(tmp_path / "upstream-src")
        self.src_files = {
            "DEPS": deps_file(dependency, old_revision),
            "chrome/VERSION": "PATCH=27\n",
            "net/a.cc": '#include "net/a.h"\n',
            "net/a.h": "int a();\nint b();\n",
            "net/BUILD.gn": "",
        }
        commit(src, self.src_files, OLD)
        self.old_time = int(git(src, "log", "-1", "--format=%ct", OLD))
        commit(src, {"chrome/VERSION": "PATCH=39\n", "DEPS": deps_file(dependency, new_revision), **src_change}, NEW)
        self.new_time = int(git(src, "log", "-1", "--format=%ct", NEW))
        self.workspace = tmp_path / "workspace"
        compare = new_repository(self.workspace / "compare" / "src")
        git(compare, "config", "remote.origin.url", src.as_uri())

    def archive(self, tmp_path: Path, patches: dict[str, str] | None = None) -> Path:
        exported = self.src_files | {f"dep/{path}": content for path, content in self.dependency_files.items()}
        return old_archive(tmp_path, exported, self.old_time, patches or {})


@pytest.fixture
def no_patches(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    (tmp_path / "patches").mkdir()
    monkeypatch.setattr(export, "PATCHES", tmp_path / "patches")


def read(archive: Path, path: str) -> str:
    with tarfile.open(archive) as tar:
        member = tar.extractfile(f"chromium-{NEW}/{path}")
        assert member is not None
        return member.read().decode()


@needs_posix
@pytest.mark.usefixtures("no_patches")
def test_files_of_the_export_come_from_the_new_tag_and_its_dependencies(tmp_path: Path) -> None:
    tree = Tree(tmp_path, {"net/a.cc": '#include "net/a.h"\nint a() { return 1; }\n'}, {"lib.h": "int f(int);\n"})
    archive = tree.archive(tmp_path)

    result = update_export.update_export(tree.workspace, archive, NEW)

    assert result == tree.workspace / "export" / NEW / f"mitto-chromium-{NEW}.tar.xz"
    assert read(result, "src/net/a.cc").endswith("return 1; }\n")
    assert read(result, "src/net/a.h") == "int a();\nint b();\n"
    assert read(result, "src/chrome/VERSION") == "PATCH=39\n"
    assert read(result, "src/dep/lib.h") == "int f(int);\n"
    assert read(result, f"src/{update_export.LASTCHANGE_COMMIT_TIME}") == str(tree.new_time)
    metadata = json.loads(read(result, "export.json"))
    assert metadata["chromium_tag"] == NEW
    assert metadata["traced_outputs"] == {}
    assert metadata["updated_from"] == {"chromium_tag": OLD, "sha256": export.sha256_of(archive)}
    with tarfile.open(result) as tar:
        assert {member.mtime for member in tar.getmembers()} == {tree.new_time}


@needs_posix
@pytest.mark.usefixtures("no_patches")
@pytest.mark.parametrize(
    ("src_change", "dependency_change", "reason"),
    [
        ({"net/BUILD.gn": "# more\n"}, {}, "net/BUILD.gn, a build file, changed"),
        ({"net/a.cc": '#include "net/a.h"\n#include "net/new.h"\n', "net/new.h": ""}, {}, "includes net/new.h"),
        ({}, {"lib.cc": '#include "dep/lib.h"\n#include <dep/other.h>\n'}, "dep/lib.cc now includes dep/other.h"),
    ],
)
def test_a_diff_that_can_add_a_file_to_the_build_asks_for_slices(
    tmp_path: Path, src_change: dict[str, str], dependency_change: dict[str, str], reason: str
) -> None:
    tree = Tree(tmp_path, src_change, dependency_change)

    with pytest.raises(update_export.UpdateError, match=reason):
        update_export.update_export(tree.workspace, tree.archive(tmp_path), NEW)

    assert not (tree.workspace / "export" / NEW).exists()


@needs_posix
@pytest.mark.usefixtures("no_patches")
def test_a_deleted_file_of_the_export_asks_for_slices(tmp_path: Path) -> None:
    tree = Tree(tmp_path, {}, {})
    git(tmp_path / "upstream-src", "tag", "-d", NEW)
    git(tmp_path / "upstream-src", "rm", "--quiet", "net/a.h")
    commit(tmp_path / "upstream-src", {}, NEW)

    with pytest.raises(update_export.UpdateError, match=r"net/a\.h is gone"):
        update_export.update_export(tree.workspace, tree.archive(tmp_path), NEW)


PATCH = (
    "Why.\n\ndiff --git a/net/a.h b/net/a.h\n--- a/net/a.h\n+++ b/net/a.h\n"
    "@@ -1,2 +1,3 @@\n int a();\n+int patched();\n int b();\n"
)


@pytest.fixture
def patch(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> dict[str, str]:
    patches = tmp_path / "patches"
    patches.mkdir()
    (patches / "a.patch").write_text(PATCH)
    monkeypatch.setattr(export, "PATCHES", patches)
    return {"a.patch": export.sha256_of(patches / "a.patch")}


@needs_posix
def test_a_patch_applies_again_to_a_file_that_upstream_changed(tmp_path: Path, patch: dict[str, str]) -> None:
    tree = Tree(tmp_path, {"net/a.h": "int a();\nint b();\n// upstream\n"}, {})
    tree.src_files["net/a.h"] = "int a();\nint patched();\nint b();\n"

    result = update_export.update_export(tree.workspace, tree.archive(tmp_path, patch), NEW)

    assert read(result, "src/net/a.h") == "int a();\nint patched();\nint b();\n// upstream\n"
    assert read(result, "patches/a.patch") == PATCH


@needs_posix
def test_a_patch_that_no_longer_applies_stops_the_update(tmp_path: Path, patch: dict[str, str]) -> None:
    tree = Tree(tmp_path, {"net/a.h": "int c();\n"}, {})
    tree.src_files["net/a.h"] = "int a();\nint patched();\nint b();\n"

    with pytest.raises(update_export.UpdateError, match=r"a\.patch no longer applies"):
        update_export.update_export(tree.workspace, tree.archive(tmp_path, patch), NEW)


@needs_posix
@pytest.mark.usefixtures("patch")
def test_an_export_made_with_other_patches_is_not_updated(tmp_path: Path) -> None:
    tree = Tree(tmp_path, {}, {})

    with pytest.raises(update_export.UpdateError, match="other patches"):
        update_export.update_export(tree.workspace, tree.archive(tmp_path), NEW)


@needs_posix
@pytest.mark.usefixtures("no_patches")
def test_builds_of_another_milestone_are_not_updated(tmp_path: Path) -> None:
    tree = Tree(tmp_path, {}, {})

    with pytest.raises(update_export.UpdateError, match="one milestone"):
        update_export.update_export(tree.workspace, tree.archive(tmp_path), "158.0.8200.1")
