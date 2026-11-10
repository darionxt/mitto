"""Tests of the Upstream lines that name where each port comes from, and of the script that compares those files.

CI has no checkout of Chromium, so the lines of the repository are checked for their form and not for their paths.
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path, PurePosixPath

import pytest

from tools.workspace import upstream

GIT = shutil.which("git")
needs_git = pytest.mark.skipif(GIT is None, reason="needs git")


def test_every_ported_file_names_its_upstream_in_the_fixed_form() -> None:
    assert upstream.form_problems(upstream.port_files()) == []


def test_the_ports_in_native_and_in_the_package_are_read() -> None:
    named = upstream.upstream_paths(upstream.port_files())

    assert named["services/network/sec_header_helpers.cc"] == [PurePosixPath("native/sec_header_helpers.cc")]
    assert PurePosixPath("src/mitto/_machine.py") in named["net/nqe/network_quality_estimator_params.cc"]
    assert PurePosixPath("native/string_hasher.h") in named["third_party/rapidhash/rapidhash.h"]


def write(directory: Path, name: str, text: str) -> Path:
    path = directory / name
    path.write_text(text, encoding="utf-8")
    return path


def test_a_port_in_cpp_names_its_upstream_files(tmp_path: Path) -> None:
    path = write(
        tmp_path,
        "port.h",
        "// Ported from services/network/a.h.\n//\n// Upstream: services/network/a.h\n"
        "#include <string>\n  // Upstream: third_party/b/c.mojom\n",
    )

    assert upstream.read_port_file(path, tmp_path) == upstream.PortFile(
        path=PurePosixPath("port.h"),
        says_ported=True,
        upstream=("services/network/a.h", "third_party/b/c.mojom"),
        malformed=(),
    )


def test_a_port_in_python_names_its_upstream_files(tmp_path: Path) -> None:
    path = write(tmp_path, "port.py", '"""A port of a.cc."""\n\n# A port of a.cc.\n# Upstream: content/a.cc\nX = 1\n')

    port_file = upstream.read_port_file(path, tmp_path)

    assert port_file is not None
    assert port_file.says_ported
    assert port_file.upstream == ("content/a.cc",)


@pytest.mark.parametrize(
    "line",
    [
        "//Upstream: net/a.cc",
        "// Upstream:net/a.cc",
        "// upstream: net/a.cc",
        "// Upstream : net/a.cc",
        "// Upstream:  net/a.cc",
        "// Upstream: net/a.cc ",
        "// Upstream: ",
        "// Upstream: /net/a.cc",
        "// Upstream: //net/a.cc",
        "// Upstream: net/",
        "// Upstream: net/../a.cc",
        "// Upstream: ./net/a.cc",
        "// Upstream: net/a.cc and net/a.h",
        "// Upstream: net\\a.cc",
    ],
)
def test_a_line_meant_as_an_upstream_line_has_to_be_in_the_form(tmp_path: Path, line: str) -> None:
    path = write(tmp_path, "port.cc", f"// Ported from net/a.cc.\n{line}\n")

    port_file = upstream.read_port_file(path, tmp_path)

    assert port_file is not None
    assert port_file.upstream == ()
    assert port_file.malformed == (f"2: {line}",)
    assert upstream.form_problems([port_file]) == [
        "port.cc says it is ported, but has no line 'Upstream: <path from src>'",
        f"port.cc:2: {line} is not 'Upstream: <path from src>'",
    ]


@pytest.mark.parametrize(
    "text",
    [
        "// Upstream asks Foo::Bar(), which is nullopt here.\n",
        'const char kLine[] = "// Upstream: net/a.cc";\n',
        "int a;  // Upstream: net/a.cc\n",
        "#include <string>  // Ported from nothing\n",
        '// - "source_port" is a port, -1 for none.\n',
    ],
)
def test_other_lines_say_nothing_of_a_port(tmp_path: Path, text: str) -> None:
    path = write(tmp_path, "file.cc", text)

    assert upstream.read_port_file(path, tmp_path) == upstream.PortFile(
        path=PurePosixPath("file.cc"), says_ported=False, upstream=(), malformed=()
    )


def test_a_file_that_says_it_is_a_port_needs_an_upstream_line(tmp_path: Path) -> None:
    path = write(tmp_path, "port.h", "// This is a port of Blink's tokenizer.\n")

    port_file = upstream.read_port_file(path, tmp_path)

    assert port_file is not None
    assert upstream.form_problems([port_file]) == [
        "port.h says it is ported, but has no line 'Upstream: <path from src>'"
    ]


def test_only_files_that_can_port_code_are_read(tmp_path: Path) -> None:
    assert upstream.read_port_file(write(tmp_path, "corpus.json", '"// Ported from a.cc"'), tmp_path) is None
    assert upstream.read_port_file(write(tmp_path, "BUILD.gn", "# Upstream: a/BUILD.gn\n"), tmp_path) is not None


def test_each_upstream_path_lists_the_files_that_port_it() -> None:
    files = [
        upstream.PortFile(PurePosixPath("native/b.h"), True, ("z/a.cc", "a/b.cc"), ()),
        upstream.PortFile(PurePosixPath("native/b.cc"), False, ("z/a.cc", "z/a.cc"), ()),
    ]

    assert upstream.upstream_paths(files) == {
        "a/b.cc": [PurePosixPath("native/b.h")],
        "z/a.cc": [PurePosixPath("native/b.h"), PurePosixPath("native/b.cc")],
    }


def git(repository: Path, *args: str) -> None:
    assert GIT is not None
    subprocess.run(
        [GIT, "-C", str(repository), "-c", "user.name=A", "-c", "user.email=a@example.com", *args],
        check=True,
        capture_output=True,
    )


@pytest.fixture
def two_tags(tmp_path: Path) -> Path:
    """A repository with the tags 1.0.0.1 and 1.0.0.2, in which a/b.cc changes, c.h goes and d.h stays."""
    repository = tmp_path / "compare" / "src"
    (repository / "a").mkdir(parents=True)
    git(repository, "init", "--quiet")
    write(repository / "a", "b.cc", "int a = 1;\n")
    write(repository, "c.h", "#define C\n")
    write(repository, "d.h", "#define D\n")
    git(repository, "add", ".")
    git(repository, "commit", "--quiet", "-m", "one")
    git(repository, "tag", "1.0.0.1")
    write(repository / "a", "b.cc", "int a = 2;\n")
    (repository / "c.h").unlink()
    git(repository, "add", "--all")
    git(repository, "commit", "--quiet", "-m", "two")
    git(repository, "tag", "1.0.0.2")
    return repository


@needs_git
def test_a_path_that_is_no_file_at_a_tag_is_missing(two_tags: Path) -> None:
    paths = ["a/b.cc", "c.h", "a", "a/*", "e.h"]

    assert upstream.missing_paths(two_tags, "1.0.0.1", paths) == ["a", "a/*", "e.h"]
    assert upstream.missing_paths(two_tags, "1.0.0.2", paths) == ["c.h", "a", "a/*", "e.h"]


@needs_git
def test_the_diff_of_each_upstream_file_is_printed(two_tags: Path, capsys: pytest.CaptureFixture[str]) -> None:
    named = {"a/b.cc": [PurePosixPath("native/b.cc")], "d.h": [PurePosixPath("native/d.h"), PurePosixPath("x.py")]}

    upstream.print_diffs(two_tags, "1.0.0.1", "1.0.0.2", named)

    output = capsys.readouterr().out
    assert output.startswith("== a/b.cc, ported in native/b.cc\ndiff --git a/a/b.cc b/a/b.cc\n")
    assert "-int a = 1;\n+int a = 2;\n" in output
    assert "== d.h, ported in native/d.h, x.py\n no changes\n" in output
    assert output.endswith("2 upstream files from 1.0.0.1 to 1.0.0.2: 1 changed, 1 not.\n")


@needs_git
@pytest.mark.skipif(sys.platform == "win32", reason="tools/workspace/compare is a bash script")
def test_the_script_fails_on_a_path_missing_at_either_tag(
    two_tags: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capfd: pytest.CaptureFixture[str]
) -> None:
    # Stands in for compare, which would fetch the tags from Chromium.
    compare = write(tmp_path, "compare-stub", '#!/bin/sh\necho compared "$1" "$2"\n')
    compare.chmod(0o755)
    monkeypatch.setattr(upstream, "COMPARE", compare)
    monkeypatch.setenv("MITTO_WORKSPACE", str(tmp_path))
    files = [
        upstream.PortFile(PurePosixPath("native/b.cc"), True, ("a/b.cc",), ()),
        upstream.PortFile(PurePosixPath("native/c.h"), True, ("c.h",), ()),
    ]
    monkeypatch.setattr(upstream, "port_files", lambda: files)

    assert upstream.main(["1.0.0.1", "1.0.0.2"]) == 1

    output = capfd.readouterr()
    assert output.out.startswith("compared 1.0.0.1 1.0.0.2\n== a/b.cc, ported in native/b.cc\n")
    assert "2 upstream files from 1.0.0.1 to 1.0.0.2: 2 changed, 0 not.\n" in output.out
    assert output.err == "upstream: error: 1.0.0.2 has no file c.h, which native/c.h names\n"


@needs_git
@pytest.mark.skipif(sys.platform == "win32", reason="tools/workspace/compare is a bash script")
def test_the_script_compares_nothing_while_a_port_names_no_upstream(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    compare = write(tmp_path, "compare-stub", "#!/bin/sh\nexit 3\n")
    compare.chmod(0o755)
    monkeypatch.setattr(upstream, "COMPARE", compare)
    monkeypatch.setattr(upstream, "port_files", lambda: [upstream.PortFile(PurePosixPath("native/a.h"), True, (), ())])

    assert upstream.main(["1.0.0.1", "1.0.0.2"]) == 1

    assert capsys.readouterr().err == (
        "upstream: error: native/a.h says it is ported, but has no line 'Upstream: <path from src>'\n"
    )


def test_the_compare_script_is_where_the_script_runs_it() -> None:
    assert upstream.COMPARE.is_file()
    assert os.access(upstream.COMPARE, os.X_OK) or sys.platform == "win32"
