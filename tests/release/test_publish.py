"""Tests of what the release workflow checks before it builds, and of the notes of its GitHub release.

Tests of a tag or of the notes run on a copy of the files they read, so they follow the version and the Supported Window
that the repository has, with a CHANGELOG.md of their own where they need an entry. They need no native library and no
network.
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
CHANGELOG = Path("CHANGELOG.md")
SOURCE = "https://github.com/darionxt/mitto"
WINDOW = sorted(WINDOWS_PERSONAS, key=int)
ENGINE = next(release for release, persona in WINDOWS_PERSONAS.items() if persona == ENGINE_RELEASE_PERSONA)


@pytest.fixture
def tree(tmp_path: Path) -> Path:
    """Return a copy of what the checks and the notes read: pyproject.toml, the engine's tag, each Baseline's note and
    CHANGELOG.md."""
    paths = [PYPROJECT, CHROMIUM_TAG, CHANGELOG, *(BASELINES / release / "chrome.txt" for release in WINDOW)]
    for path in paths:
        (tmp_path / path).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(REPOSITORY / path, tmp_path / path)
    return tmp_path


def with_version(tree: Path, version: str) -> Path:
    pyproject = tree / PYPROJECT
    text = pyproject.read_text(encoding="utf-8")
    pyproject.write_text(re.sub(r'(?m)^version = ".*"$', f'version = "{version}"', text, count=1), encoding="utf-8")
    return tree


def with_changelog(tree: Path, entries: str) -> Path:
    """Write a CHANGELOG.md of these entries, after the text that says what the file is."""
    (tree / CHANGELOG).write_text(f"# Changelog\n\nWhat changes for callers.\n\n{entries}", encoding="utf-8")
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


def test_the_notes_name_the_supported_window_and_the_engine_release(tree: Path) -> None:
    notes = release_notes("0.9.0", with_changelog(tree, "## 0.9.0\n\nA change.\n"))
    engine_build = (REPOSITORY / CHROMIUM_TAG).read_text(encoding="utf-8").strip()
    window = f"{', '.join(WINDOW[:-1])} and {WINDOW[-1]}"

    assert notes.startswith(
        f"mitto 0.9.0 presents Chrome {window}, its Supported Window, with Chrome {ENGINE} as its Engine Release. "
        f"Its network code is built from Chromium {engine_build}.\n"
    )
    assert "Install it on macOS on arm64, Linux on x86_64 or Windows on x64, with Python 3.12 or later:\n" in notes
    assert "pip install mitto==0.9.0\n" in notes
    assert "https://github.com/darionxt/mitto/blob/v0.9.0/docs/building.md#build-a-wheel" in notes


def test_the_notes_name_the_build_of_each_persona(tree: Path) -> None:
    with_changelog(tree, "## 0.9.0\n\nA change.\n")
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
    with_changelog(tree, "## 0.9.0\n\nA change.\n")
    (tree / CHROMIUM_TAG).write_text(f"{int(ENGINE) + 1}.0.1.0\n", encoding="utf-8")

    with pytest.raises(PublishError, match=rf"built from Chromium {int(ENGINE) + 1}\.0\.1\.0, which is not Chrome"):
        release_notes("0.9.0", tree)


def test_the_notes_refuse_a_persona_whose_baselines_name_no_build(tree: Path) -> None:
    with_changelog(tree, "## 0.9.0\n\nA change.\n")
    (tree / BASELINES / WINDOW[0] / "chrome.txt").unlink()

    with pytest.raises(PublishError, match=f"the Baselines of Chrome {WINDOW[0]} do not name the build"):
        release_notes("0.9.0", tree)


def test_the_notes_hold_the_entry_of_the_version_after_their_first_paragraph(tree: Path) -> None:
    entry = "Each Session does something new.\n\n- One thing.\n- Another."
    with_changelog(tree, f"## 0.10.0\n\nNot this one.\n\n## 0.9.0\n\n{entry}\n\n## 0.8.0\n\nNor this one.\n")

    paragraphs = release_notes("0.9.0", tree).split("\n\n")

    assert paragraphs[0].startswith("mitto 0.9.0 presents Chrome ")
    assert paragraphs[1:3] == entry.split("\n\n")
    assert paragraphs[3] == "Each Persona presents the build of Chrome that recorded its Baselines:"


@pytest.mark.parametrize("version", ["0.9.0rc1", "0.9.0.dev2", "0.9.0b1.dev3"])
def test_a_pre_release_holds_the_entry_of_the_release_it_comes_before(tree: Path, version: str) -> None:
    """So that a dry run shows the notes as the release will have them."""
    with_changelog(tree, "## 0.9.0\n\nA change.\n")

    notes = release_notes(version, tree)

    assert notes.startswith(f"mitto {version} presents Chrome ")
    assert "\n\nA change.\n\n" in notes


@pytest.mark.parametrize(("version", "release"), [("0.9.0", "0.9.0"), ("0.9.0rc1", "0.9.0"), ("0.8.1", "0.8.1")])
def test_a_version_with_no_entry_is_refused(tree: Path, version: str, release: str) -> None:
    """A patch needs one too, since its notes are where a caller reads what it fixes."""
    with_changelog(tree, "## 0.10.0\n\nA change.\n\n## 0.8.0\n\nAnother.\n")

    with pytest.raises(PublishError, match=rf"CHANGELOG\.md has no entry for {re.escape(release)}, and every version"):
        release_notes(version, tree)


def test_an_empty_entry_is_refused(tree: Path) -> None:
    with_changelog(tree, "## 0.9.0\n\n## 0.8.0\n\nA change.\n")

    with pytest.raises(PublishError, match=r"CHANGELOG\.md has no entry for 0\.9\.0"):
        release_notes("0.9.0", tree)


@pytest.mark.parametrize("heading", ["0.9", "v0.9.0", "0.9.0 ", "0.9.0rc1", "Unreleased", "0.9.0 (2026-11-08)"])
def test_a_heading_that_is_not_a_version_is_refused(tree: Path, heading: str) -> None:
    with_changelog(tree, f"## {heading}\n\nA change.\n\n## 0.9.0\n\nAnother.\n")

    with pytest.raises(PublishError, match=rf"CHANGELOG\.md has a heading {re.escape(repr(heading))}, where each"):
        release_notes("0.9.0", tree)


def test_two_entries_for_one_version_are_refused(tree: Path) -> None:
    with_changelog(tree, "## 0.9.0\n\nA change.\n\n## 0.9.0\n\nAnother.\n")

    with pytest.raises(PublishError, match=r"CHANGELOG\.md has two entries for 0\.9\.0"):
        release_notes("0.9.0", tree)


def test_a_heading_inside_an_entry_belongs_to_it(tree: Path) -> None:
    with_changelog(tree, "## 0.9.0\n\nA change.\n\n### More\n\nAnother.\n")

    assert "\n\nA change.\n\n### More\n\nAnother.\n\n" in release_notes("0.9.0", tree)


@pytest.mark.parametrize("version", ["0.9", "v0.9.0", "0.9.0.post1", ""])
def test_the_notes_of_something_that_is_not_a_version_are_refused(tree: Path, version: str) -> None:
    with pytest.raises(PublishError, match=f"{re.escape(version)} is not a version"):
        release_notes(version, tree)


def test_a_relative_link_in_the_entry_leads_to_the_version_s_tag(tree: Path) -> None:
    with_changelog(
        tree,
        "## 0.9.0\n\n[`machine=`](docs/api.md#machine), [the notice](MACHINE_TABLE_NOTICE), [above](#090) "
        "and [PyPI](https://pypi.org/project/mitto/).\n",
    )

    notes = release_notes("0.9.0", tree)

    assert (
        f"\n\n[`machine=`]({SOURCE}/blob/v0.9.0/docs/api.md#machine), "
        f"[the notice]({SOURCE}/blob/v0.9.0/MACHINE_TABLE_NOTICE), [above]({SOURCE}/blob/v0.9.0/CHANGELOG.md#090) "
        "and [PyPI](https://pypi.org/project/mitto/).\n\n"
    ) in notes


def test_the_version_in_pyproject_has_its_notes() -> None:
    """So a run that raises the version stops in its pull request until the version has an entry."""
    version = project_version()

    assert release_notes(version).startswith(f"mitto {version} presents Chrome ")


def test_the_command_fails_on_a_version_with_no_entry() -> None:
    command = [sys.executable, "-m", "tools.release", "notes", "999.0.0"]

    result = subprocess.run(command, cwd=REPOSITORY, capture_output=True, text=True, check=False)

    assert (result.returncode, result.stdout) == (1, "")
    assert "error: CHANGELOG.md has no entry for 999.0.0, and every version needs one" in result.stderr
