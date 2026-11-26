"""Tests of CHANGELOG.md's entries, and of gathering the files of changelog.d into them.

Tests of the order and of gathering run in a git repository of their own, whose commits stand for pull requests that
squash merges brought to main. They need no native library and no network.
"""

import shutil
import subprocess
import sys
from pathlib import Path

import pytest

from tools.release.changelog import (
    CHANGELOG,
    CHANGES_DIRECTORY,
    ChangelogError,
    change_files,
    entries,
    gather,
    in_arrival_order,
    lines_of,
    with_lines,
)

REPOSITORY = Path(__file__).parents[2]
GIT = shutil.which("git")
needs_git = pytest.mark.skipif(GIT is None, reason="needs git")

PREAMBLE = "# Changelog\n\nWhat changes for callers.\n\n"
# An entry as releases write them: a paragraph, then a list of one item a line.
NEWEST = "## 0.3.0\n\nA paragraph.\n\n- One thing.\n- Another.\n\n"
OLDER = "## 0.2.1\n\nAn older paragraph.\n\n- An older thing.\n\n## 0.2.0\n\nThe first version.\n"


def git(repository: Path, *args: str) -> None:
    assert GIT is not None
    subprocess.run(
        [GIT, "-C", str(repository), "-c", "user.name=A", "-c", "user.email=a@example.com", *args],
        check=True,
        capture_output=True,
    )


def write(root: Path, path: Path | str, text: str) -> Path:
    (root / path).parent.mkdir(parents=True, exist_ok=True)
    (root / path).write_text(text, encoding="utf-8", newline="\n")
    return Path(path)


def change(name: str) -> Path:
    return CHANGES_DIRECTORY / name


def land(repository: Path, name: str, text: str) -> Path:
    """Commit a file of changelog.d, as the squash merge of its pull request does."""
    path = write(repository, change(name), text)
    git(repository, "add", "--all")
    git(repository, "commit", "--quiet", "-m", f"add {name}")
    return path


@pytest.fixture
def repository(tmp_path: Path) -> Path:
    """Return a git repository whose first commit holds a CHANGELOG.md with an entry for 0.3.0 and older ones."""
    root = tmp_path / "repository"
    root.mkdir()
    git(root, "init", "--quiet")
    write(root, CHANGELOG, PREAMBLE + NEWEST + OLDER)
    git(root, "add", "--all")
    git(root, "commit", "--quiet", "-m", "start")
    return root


# Entries.


def test_the_entries_are_in_the_order_of_the_file_after_the_preamble() -> None:
    text = PREAMBLE + NEWEST + OLDER

    found = entries(text)

    assert [entry.version for entry in found] == ["0.3.0", "0.2.1", "0.2.0"]
    assert text[found[0].heading : found[0].start] == "## 0.3.0"
    assert text[found[0].start : found[0].end] == "\n\nA paragraph.\n\n- One thing.\n- Another.\n\n"
    assert text[found[-1].start : found[-1].end] == "\n\nThe first version.\n"


@pytest.mark.parametrize("heading", ["0.9", "v0.9.0", "0.9.0 ", "0.9.0rc1", "Unreleased", "0.9.0 (2026-11-08)"])
def test_a_heading_that_is_not_a_version_is_refused(heading: str) -> None:
    with pytest.raises(ChangelogError, match=r"CHANGELOG\.md has a heading .*, where each names a version"):
        entries(f"{PREAMBLE}## {heading}\n\nA change.\n\n{OLDER}")


def test_two_entries_for_one_version_are_refused() -> None:
    with pytest.raises(ChangelogError, match=r"CHANGELOG\.md has two entries for 0\.2\.1"):
        entries(PREAMBLE + OLDER + "\n## 0.2.1\n\nAgain.\n")


# Where the lines go.


def test_lines_continue_the_list_of_an_entry_that_already_has_lines() -> None:
    text = with_lines(PREAMBLE + NEWEST + OLDER, "0.3.0", ["- A third thing.", "- A fourth."])

    assert text == PREAMBLE + NEWEST.replace("- Another.\n", "- Another.\n- A third thing.\n- A fourth.\n") + OLDER


def test_lines_start_a_list_after_an_entry_that_has_only_a_paragraph() -> None:
    text = with_lines(PREAMBLE + OLDER, "0.2.0", ["- A thing."])

    assert text == PREAMBLE + OLDER.replace("The first version.\n", "The first version.\n\n- A thing.\n")


def test_lines_are_the_text_of_an_entry_that_has_only_its_heading() -> None:
    text = with_lines(f"{PREAMBLE}## 0.3.0\n\n{OLDER}", "0.3.0", ["- A thing."])

    assert text == f"{PREAMBLE}## 0.3.0\n\n- A thing.\n\n{OLDER}"


def test_a_patch_gets_an_entry_above_the_version_it_follows() -> None:
    text = with_lines(PREAMBLE + NEWEST + OLDER, "0.3.1", ["- A fix."])

    assert text == f"{PREAMBLE}## 0.3.1\n\n- A fix.\n\n{NEWEST}{OLDER}"


def test_a_patch_gets_its_entry_below_a_newer_version_s() -> None:
    """Entries stay newest first, so a patch that ships before the next minor version goes below that version's entry."""
    text = with_lines(PREAMBLE + NEWEST + OLDER, "0.2.2", ["- A fix."])

    assert text == f"{PREAMBLE}{NEWEST}## 0.2.2\n\n- A fix.\n\n{OLDER}"


def test_a_version_older_than_every_entry_gets_one_at_the_end() -> None:
    text = with_lines(PREAMBLE + OLDER, "0.1.0", ["- A thing."])

    assert text == f"{PREAMBLE}{OLDER}\n## 0.1.0\n\n- A thing.\n"


def test_a_changelog_with_no_entry_gets_its_first() -> None:
    assert with_lines(PREAMBLE, "0.1.0", ["- A thing."]) == f"{PREAMBLE}## 0.1.0\n\n- A thing.\n"


def test_no_lines_change_nothing() -> None:
    text = PREAMBLE + NEWEST + OLDER

    assert with_lines(text, "0.3.0", []) == text
    assert with_lines(text, "0.4.0", []) == text


@pytest.mark.parametrize("version", ["0.3", "v0.3.0", "0.3.0rc1", "0.03.0", ""])
def test_lines_for_something_that_is_not_a_release_are_refused(version: str) -> None:
    with pytest.raises(ChangelogError, match=r"is not the version of a release, such as 0\.1\.0"):
        with_lines(PREAMBLE + NEWEST, version, ["- A thing."])


# The files.


def test_the_files_are_by_name_and_leave_out_hidden_ones(tmp_path: Path) -> None:
    for name in ["b.md", "a.md", ".DS_Store", "c.txt"]:
        write(tmp_path, change(name), "- A thing.\n")

    assert change_files(tmp_path) == [change("a.md"), change("b.md"), change("c.txt")]


def test_no_directory_holds_no_file(tmp_path: Path) -> None:
    assert change_files(tmp_path) == []


def link_changes_to(root: Path, target: Path) -> None:
    try:
        (root / CHANGES_DIRECTORY).symlink_to(target, target_is_directory=True)
    except OSError as error:
        pytest.skip(f"cannot make a link here: {error}")


@needs_git
def test_a_link_for_the_directory_is_refused_before_anything_goes(repository: Path, tmp_path: Path) -> None:
    """Gathering deletes the files, which a link could put outside the repository."""
    outside = tmp_path / "outside"
    write(outside, "a.md", "- A thing.\n")
    link_changes_to(repository, outside)

    with pytest.raises(ChangelogError, match=r"changelog\.d is a link, where it should be a directory"):
        gather("0.3.0", repository)

    assert (outside / "a.md").exists()


def test_a_file_gives_its_items_without_blank_lines_or_the_spaces_after_them(tmp_path: Path) -> None:
    path = write(tmp_path, change("a.md"), "\n- One thing, with [a link](docs/api.md#machine).  \n\n- Another.\n\n")

    assert lines_of(path, tmp_path) == ["- One thing, with [a link](docs/api.md#machine).", "- Another."]


@pytest.mark.parametrize(
    ("text", "error"),
    [
        (
            "- One thing.\nA paragraph.\n",
            r'changelog\.d/a\.md:2 is not an item of a list, a line that starts with "- "',
        ),
        ("  - Indented.\n", r"changelog\.d/a\.md:1 is not an item"),
        ("-No space.\n", r"changelog\.d/a\.md:1 is not an item"),
        ("## 0.3.0\n", r"changelog\.d/a\.md:1 is not an item"),
        ("\n\n", r"changelog\.d/a\.md holds no line for callers"),
    ],
)
def test_a_file_of_anything_but_items_is_refused(tmp_path: Path, text: str, error: str) -> None:
    path = write(tmp_path, change("a.md"), text)

    with pytest.raises(ChangelogError, match=error):
        lines_of(path, tmp_path)


def test_a_file_that_is_not_markdown_or_no_file_is_refused(tmp_path: Path) -> None:
    write(tmp_path, change("a.txt"), "- A thing.\n")
    (tmp_path / change("b.md")).mkdir()

    for name in ["a.txt", "b.md"]:
        with pytest.raises(ChangelogError, match=rf"changelog\.d/{name} is not a file of lines for callers"):
            lines_of(change(name), tmp_path)


def test_each_file_of_the_repository_gives_lines() -> None:
    """So that a pull request with a file the release could not gather fails in its own CI."""
    for path in change_files(REPOSITORY):
        assert lines_of(path, REPOSITORY)


# The order that the files came to main.


@needs_git
def test_the_files_come_in_the_order_that_commits_added_them(repository: Path) -> None:
    for name in ["c.md", "a.md", "b.md"]:
        land(repository, name, f"- {name}\n")

    assert in_arrival_order(change_files(repository), repository) == [change("c.md"), change("a.md"), change("b.md")]


@needs_git
def test_a_later_change_to_a_file_keeps_its_place(repository: Path) -> None:
    land(repository, "b.md", "- B.\n")
    land(repository, "a.md", "- A.\n")
    land(repository, "b.md", "- B, changed.\n")

    assert in_arrival_order(change_files(repository), repository) == [change("b.md"), change("a.md")]


@needs_git
def test_a_file_added_again_counts_from_the_last_time(repository: Path) -> None:
    """As when a release gathered a branch's file, and a later branch of the same name writes it again."""
    land(repository, "b.md", "- B.\n")
    (repository / change("b.md")).unlink()
    git(repository, "commit", "--quiet", "--all", "-m", "gather")
    land(repository, "a.md", "- A.\n")
    land(repository, "b.md", "- B again.\n")

    assert in_arrival_order(change_files(repository), repository) == [change("a.md"), change("b.md")]


@needs_git
def test_a_file_written_again_after_it_went_comes_after_the_committed_ones(repository: Path) -> None:
    """Its old addition is not when it came, as long as no commit holds it again."""
    land(repository, "b.md", "- B.\n")
    (repository / change("b.md")).unlink()
    git(repository, "commit", "--quiet", "--all", "-m", "gather")
    land(repository, "a.md", "- A.\n")
    write(repository, change("b.md"), "- B again, not committed.\n")

    assert in_arrival_order(change_files(repository), repository) == [change("a.md"), change("b.md")]


@needs_git
def test_a_renamed_file_counts_from_its_new_name(repository: Path) -> None:
    land(repository, "a.md", "- A.\n")
    land(repository, "b.md", "- B.\n")
    git(repository, "mv", change("a.md").as_posix(), change("c.md").as_posix())
    git(repository, "commit", "--quiet", "-m", "rename")

    assert in_arrival_order(change_files(repository), repository) == [change("b.md"), change("c.md")]


@needs_git
def test_files_of_one_commit_and_files_of_none_come_by_name_after_the_others(repository: Path) -> None:
    write(repository, change("d.md"), "- D.\n")
    write(repository, change("c.md"), "- C.\n")
    git(repository, "add", "--all")
    git(repository, "commit", "--quiet", "-m", "two files")
    land(repository, "e.md", "- E.\n")
    write(repository, change("b.md"), "- Not committed.\n")
    write(repository, change("a.md"), "- Not committed either.\n")

    assert in_arrival_order(change_files(repository), repository) == [
        change("c.md"),
        change("d.md"),
        change("e.md"),
        change("a.md"),
        change("b.md"),
    ]


@needs_git
def test_two_branches_that_each_add_lines_merge_with_no_conflict(repository: Path) -> None:
    """Each writes a file of its own, where both used to add to the end of one list in CHANGELOG.md."""
    git(repository, "branch", "-M", "main")
    for branch in ["feat/frame-storage", "fix/a-fix"]:
        git(repository, "switch", "--quiet", "-c", branch, "main")
        land(repository, f"{branch.split('/')[-1]}.md", f"- {branch}.\n")
    git(repository, "switch", "--quiet", "main")
    for branch in ["fix/a-fix", "feat/frame-storage"]:
        git(repository, "merge", "--quiet", "--squash", branch)
        git(repository, "commit", "--quiet", "-m", f"merge {branch}")

    assert gather("0.3.0", repository) == [change("a-fix.md"), change("frame-storage.md")]
    assert "- Another.\n- fix/a-fix.\n- feat/frame-storage.\n\n## 0.2.1" in (repository / CHANGELOG).read_text(
        encoding="utf-8"
    )


def test_the_order_needs_a_git_repository(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    # So that git looks for no repository above the test's directory, which may sit in one.
    monkeypatch.setenv("GIT_CEILING_DIRECTORIES", str(tmp_path.parent))
    write(tmp_path, change("a.md"), "- A.\n")

    with pytest.raises(ChangelogError, match=r"git cannot read when the files of changelog\.d came"):
        in_arrival_order(change_files(tmp_path), tmp_path)


# Gathering.


@needs_git
def test_gathering_writes_each_file_s_lines_in_the_order_the_files_came_and_deletes_them(repository: Path) -> None:
    land(repository, "frame-storage.md", "- A Page keeps a frame's storage.\n- And its cookies.\n")
    land(repository, "a-fix.md", "- A fix.\n")

    gathered = gather("0.3.0", repository)

    assert gathered == [change("frame-storage.md"), change("a-fix.md")]
    assert change_files(repository) == []
    added = "- A Page keeps a frame's storage.\n- And its cookies.\n- A fix.\n"
    assert (repository / CHANGELOG).read_text(encoding="utf-8") == PREAMBLE + NEWEST.replace(
        "- Another.\n", f"- Another.\n{added}"
    ) + OLDER


@needs_git
def test_gathering_into_a_new_entry_writes_its_heading(repository: Path) -> None:
    """As a minor version that a run makes does, before its first paragraph is written."""
    land(repository, "b.md", "- B.\n")
    land(repository, "a.md", "- A.\n")

    gather("0.4.0", repository)

    assert (repository / CHANGELOG).read_text(
        encoding="utf-8"
    ) == f"{PREAMBLE}## 0.4.0\n\n- B.\n- A.\n\n{NEWEST}{OLDER}"


@needs_git
def test_a_patch_gathers_the_files_of_its_own_branch(repository: Path) -> None:
    """A patch's pull request gathers what its branch holds into the patch's entry, as a run's does."""
    git(repository, "tag", "v0.3.0")
    git(repository, "switch", "--quiet", "-c", "release-0.3.1")
    land(repository, "a-fix.md", "- A fix of a security bug.\n")

    assert gather("0.3.1", repository) == [change("a-fix.md")]
    assert (repository / CHANGELOG).read_text(encoding="utf-8") == (
        f"{PREAMBLE}## 0.3.1\n\n- A fix of a security bug.\n\n{NEWEST}{OLDER}"
    )


@needs_git
def test_gathering_an_empty_directory_changes_nothing(repository: Path) -> None:
    (repository / CHANGES_DIRECTORY).mkdir()
    before = (repository / CHANGELOG).read_bytes()

    assert gather("0.3.0", repository) == []
    assert (repository / CHANGELOG).read_bytes() == before


@needs_git
def test_gathering_with_a_file_it_cannot_read_changes_nothing(repository: Path) -> None:
    land(repository, "a.md", "- A.\n")
    land(repository, "b.md", "Not an item.\n")
    before = (repository / CHANGELOG).read_bytes()

    with pytest.raises(ChangelogError, match=r"changelog\.d/b\.md:1 is not an item"):
        gather("0.3.0", repository)

    assert (repository / CHANGELOG).read_bytes() == before
    assert change_files(repository) == [change("a.md"), change("b.md")]


@needs_git
def test_gathering_refuses_what_is_not_the_version_of_a_release(repository: Path) -> None:
    land(repository, "a.md", "- A.\n")

    with pytest.raises(ChangelogError, match=r"0\.3\.0rc1 is not the version of a release"):
        gather("0.3.0rc1", repository)

    assert change_files(repository) == [change("a.md")]


def test_the_command_refuses_what_is_not_the_version_of_a_release() -> None:
    """It stops before it reads anything, so it can run on this repository's own files."""
    command = [sys.executable, "-m", "tools.release", "changelog", "0.4"]

    result = subprocess.run(command, cwd=REPOSITORY, capture_output=True, text=True, check=False)

    assert (result.returncode, result.stdout) == (1, "")
    assert "error: 0.4 is not the version of a release, such as 0.1.0" in result.stderr
