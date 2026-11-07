"""Tests of what the release workflow checks before it builds, and of the notes of its GitHub release.

Tests of a tag or of the notes run on a copy of the files they read, so they follow the version and the Supported Window
that the repository has. They need no native library and no network.
"""

import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

from mitto._persona import ENGINE_RELEASE_PERSONA, WINDOWS_PERSONAS
from tools.release.publish import PublishError, dry_run_version, project_version, release_notes, tag_version

REPOSITORY = Path(__file__).parents[2]
PYPROJECT = Path("pyproject.toml")
CHROMIUM_TAG = Path("tools/workspace/chromium_tag")
BASELINES = Path("tests/baselines")
WINDOW = sorted(WINDOWS_PERSONAS, key=int)
ENGINE = next(release for release, persona in WINDOWS_PERSONAS.items() if persona == ENGINE_RELEASE_PERSONA)


@pytest.fixture
def tree(tmp_path: Path) -> Path:
    """Return a copy of what the checks and the notes read: pyproject.toml, the engine's tag and each Baseline's note."""
    paths = [PYPROJECT, CHROMIUM_TAG, *(BASELINES / release / "chrome.txt" for release in WINDOW)]
    for path in paths:
        (tmp_path / path).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(REPOSITORY / path, tmp_path / path)
    return tmp_path


def with_version(tree: Path, version: str) -> Path:
    pyproject = tree / PYPROJECT
    text = pyproject.read_text(encoding="utf-8")
    pyproject.write_text(re.sub(r'(?m)^version = ".*"$', f'version = "{version}"', text, count=1), encoding="utf-8")
    return tree


def test_the_project_version_is_the_one_in_pyproject(tree: Path) -> None:
    assert project_version(with_version(tree, "1.2.3")) == "1.2.3"


def test_a_tag_publishes_the_version_in_pyproject(tree: Path) -> None:
    assert tag_version("v0.12.3", with_version(tree, "0.12.3")) == "0.12.3"


@pytest.mark.parametrize("tag", ["v0.1.1", "v0.2.0", "v1.1.0"])
def test_a_tag_of_another_version_is_refused(tree: Path, tag: str) -> None:
    with pytest.raises(PublishError, match=rf"the tag {tag} is not the version in pyproject\.toml, 0\.1\.0"):
        tag_version(tag, with_version(tree, "0.1.0"))


@pytest.mark.parametrize("tag", ["0.1.0", "v0.1", "v0.1.0.0", "v0.1.0rc1", "v0.1.0-1", "v00.1.0", "V0.1.0", "v0.1.0 "])
def test_a_tag_that_is_not_v_and_three_numbers_is_refused(tree: Path, tag: str) -> None:
    with pytest.raises(PublishError, match=r"is not vX\.Y\.Z"):
        tag_version(tag, with_version(tree, "0.1.0"))


def test_the_command_fails_on_a_tag_of_another_version() -> None:
    """The workflow runs the command before anything builds, so such a tag stops the release there."""
    command = [sys.executable, "-m", "tools.release", "version", "--tag", "v999.0.0"]

    result = subprocess.run(command, cwd=REPOSITORY, capture_output=True, text=True, check=False)

    assert (result.returncode, result.stdout) == (1, "")
    assert "error: the tag v999.0.0 is not the version in pyproject.toml" in result.stderr


@pytest.mark.parametrize("version", ["0.1.0a1", "0.1.0b2", "0.1.0rc1", "0.1.0.dev1", "0.1.0rc1.dev2", "1.10.0.dev0"])
def test_a_dry_run_publishes_a_pre_release(version: str) -> None:
    assert dry_run_version(version) == version


@pytest.mark.parametrize(
    "version",
    ["0.1.0", "0.1.0.post1", "0.1.0-rc1", "0.1.0RC1", "0.1.0.rc1", "0.1.0c1", "0.1.0.dev", "v0.1.0rc1", "0.1rc1", ""],
)
def test_a_dry_run_refuses_a_release_or_a_version_out_of_its_normal_form(version: str) -> None:
    with pytest.raises(PublishError, match="a dry run publishes a pre-release"):
        dry_run_version(version)


def test_the_notes_name_the_supported_window_and_the_engine_release() -> None:
    notes = release_notes("0.9.0")
    engine_build = (REPOSITORY / CHROMIUM_TAG).read_text(encoding="utf-8").strip()
    window = f"{', '.join(WINDOW[:-1])} and {WINDOW[-1]}"

    assert notes.startswith(
        f"mitto 0.9.0 presents Chrome {window}, its Supported Window, with Chrome {ENGINE} as its Engine Release. "
        f"Its network code is built from Chromium {engine_build}.\n"
    )
    assert "pip install mitto==0.9.0\n" in notes
    assert "https://github.com/darionxt/mitto/blob/v0.9.0/docs/building.md#build-a-wheel" in notes


def test_the_notes_name_the_build_of_each_persona(tree: Path) -> None:
    builds: dict[str, str] = {}
    for release in WINDOW:
        note = tree / BASELINES / release / "chrome.txt"
        builds[release] = f"{release}.0.1234.5"
        note.write_text(
            re.sub(r"(?m)^Chrome: .*$", f"Chrome: {builds[release]}", note.read_text(encoding="utf-8")),
            encoding="utf-8",
        )

    notes = release_notes("0.9.0", tree)

    assert [f"- Chrome {release}: {build}" for release, build in builds.items()] == re.findall(r"(?m)^- .*$", notes)


def test_the_notes_refuse_an_engine_of_another_milestone(tree: Path) -> None:
    (tree / CHROMIUM_TAG).write_text(f"{int(ENGINE) + 1}.0.1.0\n", encoding="utf-8")

    with pytest.raises(PublishError, match=rf"built from Chromium {int(ENGINE) + 1}\.0\.1\.0, which is not Chrome"):
        release_notes("0.9.0", tree)


def test_the_notes_refuse_a_persona_whose_baselines_name_no_build(tree: Path) -> None:
    (tree / BASELINES / WINDOW[0] / "chrome.txt").unlink()

    with pytest.raises(PublishError, match=f"the Baselines of Chrome {WINDOW[0]} do not name the build"):
        release_notes("0.9.0", tree)
