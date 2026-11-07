"""Tests of the script that drops the oldest Chrome Release, on a copy of the repository's tree. They need no native library.

Each test copies the tree, gives the copy the Persona and Baselines of a new Engine Release, as the routine does before it
drops the oldest, and drops it there. So the tests follow the Supported Window that the repository has.
"""

import json
import os
import re
import shutil
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path

import pytest

from tests.test_docs import lines_naming_chrome_releases, supported_window
from tools.parity import Gap, dumps_known_gaps, loads_known_gaps
from tools.release.drop import DropError, Named, drop_oldest, releases, still_named

REPOSITORY = Path(__file__).parents[2]
WINDOW = releases(REPOSITORY)
OLDEST = WINDOW[0]
NEW = str(int(WINDOW[-1]) + 1)

PERSONAS = Path("src/mitto/personas")
PERSONA_MODULE = Path("src/mitto/_persona.py")
BASELINES = Path("tests/baselines")
KNOWN_GAPS = BASELINES / "known_gaps.json"
README = Path("README.md")

type Layout = Callable[[list[str]], str]
"""What writes WINDOWS_PERSONAS for some Chrome Releases, as ruff formats it on one line or on a line for each."""


def one_line(window: list[str]) -> str:
    return "WINDOWS_PERSONAS = {" + ", ".join(f'"{release}": CHROME_{release}_WINDOWS' for release in window) + "}\n"


def a_line_each(window: list[str]) -> str:
    return (
        "WINDOWS_PERSONAS = {\n"
        + "".join(f'    "{release}": CHROME_{release}_WINDOWS,\n' for release in window)
        + "}\n"
    )


@pytest.fixture
def tree(tmp_path: Path) -> Path:
    """Return a copy of the repository's code, data and README."""
    ignored = shutil.ignore_patterns("__pycache__", ".pytest_cache", "*.dylib", "*.so")
    for directory in ("src", "tests", "tools"):
        shutil.copytree(REPOSITORY / directory, tmp_path / directory, ignore=ignored)
    shutil.copy(REPOSITORY / README, tmp_path / README)
    return tmp_path


def add_new_engine_release(tree: Path, layout: Layout = one_line, *, engine: str = NEW) -> None:
    """Give the tree the Persona and the Baselines of the milestone after the newest, as the routine's earlier steps do."""
    newest = WINDOW[-1]
    shutil.copy(tree / PERSONAS / f"chrome_{newest}_windows.json", tree / PERSONAS / f"chrome_{NEW}_windows.json")
    shutil.copytree(tree / BASELINES / newest, tree / BASELINES / NEW)
    module = tree / PERSONA_MODULE
    text = module.read_text()
    text = text.replace(
        f'    "CHROME_{newest}_WINDOWS",\n', f'    "CHROME_{newest}_WINDOWS",\n    "CHROME_{NEW}_WINDOWS",\n'
    )
    text = text.replace(
        f'CHROME_{newest}_WINDOWS = "chrome_{newest}_windows"\n',
        f'CHROME_{newest}_WINDOWS = "chrome_{newest}_windows"\nCHROME_{NEW}_WINDOWS = "chrome_{NEW}_windows"\n',
    )
    text = re.sub(r"^WINDOWS_PERSONAS = \{.*?\}\n", lambda _: layout([*WINDOW, NEW]), text, flags=re.M | re.S)
    text = re.sub(
        r"^ENGINE_RELEASE_PERSONA = .*$", f"ENGINE_RELEASE_PERSONA = CHROME_{engine}_WINDOWS", text, flags=re.M
    )
    module.write_text(text)


def persona_module_of(tree: Path) -> dict[str, object]:
    """Import the tree's src/mitto/_persona.py in a new interpreter, and return what it names."""
    code = (
        "import json, mitto._persona as p\n"
        "print(json.dumps({'file': p.__file__, 'windows': p.WINDOWS_PERSONAS, 'engine': p.ENGINE_RELEASE_PERSONA,"
        " 'all': p.__all__, 'undefined': [name for name in p.__all__ if not hasattr(p, name)]}))"
    )
    environment = {**os.environ, "PYTHONPATH": str(tree / "src")}
    result = subprocess.run([sys.executable, "-c", code], env=environment, capture_output=True, text=True, check=True)
    return json.loads(result.stdout)


def snapshot(tree: Path) -> dict[str, bytes]:
    return {str(path.relative_to(tree)): path.read_bytes() for path in sorted(tree.rglob("*")) if path.is_file()}


@pytest.mark.parametrize("layout", [one_line, a_line_each])
def test_the_oldest_chrome_release_leaves_the_code_and_the_data(tree: Path, layout: Layout) -> None:
    add_new_engine_release(tree, layout)

    dropped = drop_oldest(tree)

    assert dropped.release == OLDEST
    assert releases(tree) == [*WINDOW[1:], NEW]
    assert not (tree / PERSONAS / f"chrome_{OLDEST}_windows.json").exists()
    assert not (tree / BASELINES / OLDEST).exists()
    assert sorted(path.name for path in (tree / BASELINES).iterdir() if path.is_dir()) == [*WINDOW[1:], NEW]
    module = persona_module_of(tree)
    assert module["file"] == str(tree / PERSONA_MODULE)
    assert module["windows"] == {release: f"chrome_{release}_windows" for release in [*WINDOW[1:], NEW]}
    assert module["engine"] == f"chrome_{NEW}_windows"
    assert module["undefined"] == []
    assert f"CHROME_{OLDEST}_WINDOWS" not in (tree / PERSONA_MODULE).read_text()
    assert [supported_window(line) for line in lines_naming_chrome_releases((tree / README).read_text())] == [
        ([*WINDOW[1:], NEW], NEW)
    ]


def test_only_the_line_that_names_the_supported_window_changes_in_the_readme(tree: Path) -> None:
    add_new_engine_release(tree)
    before = (tree / README).read_text().splitlines()

    drop_oldest(tree)

    after = (tree / README).read_text().splitlines()
    assert len(after) == len(before)
    assert [(old, new) for old, new in zip(before, after, strict=True) if old != new] == [
        (
            f"This version presents Chrome {', '.join(WINDOW[:-1])} and {WINDOW[-1]}, "
            f"with Chrome {WINDOW[-1]} as its Engine Release.",
            f"This version presents Chrome {', '.join(WINDOW[1:])} and {NEW}, with Chrome {NEW} as its Engine Release.",
        )
    ]


def test_only_the_lines_that_name_the_oldest_leave_the_persona_module(tree: Path) -> None:
    add_new_engine_release(tree, a_line_each)
    before = (tree / PERSONA_MODULE).read_text().splitlines()

    drop_oldest(tree)

    after = (tree / PERSONA_MODULE).read_text().splitlines()
    assert after == [line for line in before if f"CHROME_{OLDEST}_WINDOWS" not in line]


def test_no_code_names_the_oldest_chrome_release_once_it_has_gone(tree: Path) -> None:
    add_new_engine_release(tree)

    dropped = drop_oldest(tree)

    # The tests of the release tools hold what Chromium Dash said on one day, which is a record and stays.
    assert [named for named in dropped.still_named if not named.path.is_relative_to("tests/release")] == []


def test_the_known_gaps_of_the_oldest_chrome_release_leave_the_list(tree: Path) -> None:
    add_new_engine_release(tree)
    gaps = [
        Gap(OLDEST, "navigation", "h2", "request.headers.user-agent", "a", "b"),
        Gap(OLDEST, "fetch", "h3", "tls.cipher_suites", ["A"], None),
        Gap(NEW, "image", "http/1.1", "request.headers.accept", None, "*/*"),
    ]
    (tree / KNOWN_GAPS).write_text(dumps_known_gaps(gaps))

    dropped = drop_oldest(tree)

    assert dropped.gaps == 2
    assert loads_known_gaps((tree / KNOWN_GAPS).read_text()) == gaps[2:]


def test_a_supported_window_of_three_is_left_as_it_is(tree: Path) -> None:
    before = snapshot(tree)

    with pytest.raises(DropError, match="Write the Persona of the new Engine Release first"):
        drop_oldest(tree)

    assert snapshot(tree) == before


def test_a_persona_module_that_names_the_oldest_elsewhere_leaves_the_tree_as_it_is(tree: Path) -> None:
    add_new_engine_release(tree, engine=OLDEST)
    before = snapshot(tree)

    with pytest.raises(DropError, match=f"still names CHROME_{OLDEST}_WINDOWS"):
        drop_oldest(tree)

    assert snapshot(tree) == before


def test_a_persona_module_without_the_lines_it_expects_leaves_the_tree_as_it_is(tree: Path) -> None:
    add_new_engine_release(tree)
    module = tree / PERSONA_MODULE
    module.write_text(module.read_text().replace(f'    "CHROME_{OLDEST}_WINDOWS",\n', ""))
    before = snapshot(tree)

    with pytest.raises(DropError, match="change it by hand"):
        drop_oldest(tree)

    assert snapshot(tree) == before


def test_an_engine_release_that_is_not_the_newest_leaves_the_tree_as_it_is(tree: Path) -> None:
    add_new_engine_release(tree, engine=WINDOW[-1])
    before = snapshot(tree)

    with pytest.raises(
        DropError, match=f"the Engine Release is Chrome {WINDOW[-1]}, and the newest Persona is of Chrome {NEW}"
    ):
        drop_oldest(tree)

    assert snapshot(tree) == before


def test_a_readme_without_the_line_that_names_the_window_leaves_the_tree_as_it_is(tree: Path) -> None:
    add_new_engine_release(tree)
    readme = tree / README
    readme.write_text(readme.read_text().replace("This version presents Chrome", "This version shows Chrome"))
    before = snapshot(tree)

    with pytest.raises(DropError, match=r"README\.md does not have one line that names the Supported Window"):
        drop_oldest(tree)

    assert snapshot(tree) == before


def test_a_tree_without_a_readme_is_left_as_it_is(tree: Path) -> None:
    add_new_engine_release(tree)
    (tree / README).unlink()
    before = snapshot(tree)

    with pytest.raises(DropError, match=r"the tree has no README\.md"):
        drop_oldest(tree)

    assert snapshot(tree) == before


def test_an_oldest_chrome_release_without_baselines_leaves_the_tree_as_it_is(tree: Path) -> None:
    add_new_engine_release(tree)
    shutil.rmtree(tree / BASELINES / OLDEST)
    before = snapshot(tree)

    with pytest.raises(DropError, match=f"Chrome {OLDEST} has no Baselines"):
        drop_oldest(tree)

    assert snapshot(tree) == before


def test_code_that_names_a_chrome_release_is_found_and_the_number_alone_is_not(tmp_path: Path) -> None:
    lines = {
        "src/a.py": ['PERSONA = "chrome_150_windows"', "SIZE = 150", 'LABEL = "150"'],
        "tests/b.py": ["NAME = CHROME_150_WINDOWS", 'BUILD = "150.0.7000.12"', 'OTHER = "1150.0.7000.12"'],
        "tools/c.ps1": ["$agent = 'Chrome/150.0.0.0'", "$other = 'Chrome/1500.0.0.0'"],
        "tools/d.json": ['"chrome_150_windows"'],
        "docs/e.md": ["Chrome 150, chrome_150_windows"],
    }
    for path, text in lines.items():
        (tmp_path / path).parent.mkdir(parents=True, exist_ok=True)
        (tmp_path / path).write_text("\n".join(text) + "\n")

    assert still_named("150", tmp_path) == [
        Named(Path("src/a.py"), 1, 'PERSONA = "chrome_150_windows"'),
        Named(Path("tests/b.py"), 1, "NAME = CHROME_150_WINDOWS"),
        Named(Path("tests/b.py"), 2, 'BUILD = "150.0.7000.12"'),
        Named(Path("tools/c.ps1"), 1, "$agent = 'Chrome/150.0.0.0'"),
    ]
