"""Tests of reading Chromium's repositories from their Gitiles servers, with no network."""

import base64
import json
import urllib.parse

import pytest

from tests.release.saved import saved
from tools.release.gitiles import CHROMIUM_URL, Change, commits_between, submodule_urls
from tools.release.web import Fetch, WebError


def answering(*pages: object) -> tuple[Fetch, list[str]]:
    """Return what gives these logs in turn, behind Gitiles' prefix, and the URLs it was asked for."""
    asked: list[str] = []

    def fetch(url: str) -> bytes:
        asked.append(url)
        return b")]}'\n" + json.dumps(pages[len(asked) - 1]).encode()

    return fetch, asked


def commit(sha: str, message: str, *changes: dict[str, object]) -> dict[str, object]:
    return {"commit": sha, "message": message, "tree_diff": list(changes)}


def change(kind: str, old_path: str, new_path: str, mode: int = 0o100644) -> dict[str, object]:
    return {
        "type": kind,
        "old_id": "1" * 40,
        "old_mode": mode,
        "old_path": old_path,
        "new_id": "2" * 40,
        "new_mode": mode,
        "new_path": new_path,
    }


def test_the_commits_between_two_builds_are_those_of_the_day() -> None:
    commits = commits_between(CHROMIUM_URL, "157.0.8103.27", "157.0.8103.39", saved)

    assert len(commits) == 95
    assert commits[0].subject == "Incrementing VERSION to 157.0.8103.39"
    (fix,) = [found for found in commits if found.id.startswith("d1676d6c0e7d")]
    assert fix.subject == "[M157] HttpCache::Transaction reject overlong range responses"
    assert fix.bugs == ("572055997", "578797822")
    assert [found.path for found in fix.changes] == [
        "net/http/http_cache_transaction.cc",
        "net/http/http_cache_unittest.cc",
        "net/http/http_cache_writers.cc",
        "net/http/http_cache_writers.h",
        "net/http/partial_data.cc",
        "net/http/partial_data.h",
    ]


def test_a_submodule_is_a_change_of_its_own_kind_with_its_commits() -> None:
    commits = commits_between(CHROMIUM_URL, "157.0.8103.27", "157.0.8103.39", saved)

    (roll,) = [found for found in commits if found.id.startswith("e36354ed0d")]
    assert [found.path for found in roll.changes] == ["DEPS", "third_party/angle"]
    assert [found.submodule for found in roll.changes] == [False, True]
    assert roll.changes[1].old_id == "7bf673433dd1a7602a9323608d7e3fb1583dfd1e"
    assert roll.changes[1].new_id == "751417514a521b2e92d6928260c62122138824d3"


def test_the_question_asks_for_the_changes_of_each_commit() -> None:
    fetch, asked = answering({"log": []})

    commits_between("https://boringssl.googlesource.com/boringssl.git", "a" * 40, "b" * 40, fetch)

    (url,) = asked
    assert url.startswith(f"https://boringssl.googlesource.com/boringssl/+log/{'a' * 40}..{'b' * 40}?")
    assert urllib.parse.parse_qs(urllib.parse.urlsplit(url).query) == {
        "format": ["JSON"],
        "name-status": ["1"],
        "n": ["1000"],
    }


@pytest.mark.parametrize(
    ("message", "bugs"),
    [
        ("Fix\n\nBug: 572055997\n", ("572055997",)),
        ("Fix\n\nBug: 578797822,572055997\n", ("578797822", "572055997")),
        ("Fix\n\n> Bug: 572055997\n\nBug: 578797822, 572055997\n", ("572055997", "578797822")),
        ("Fix\n\nBug: chromium:573933450, b:12345678\n", ("573933450", "12345678")),
        ("Fix\n\nFixed: 568826881\n", ("568826881",)),
        ("Fix 572055997 in the subject\n\nChange-Id: I7a0ca8652462fa890cf04944c21e0674d3d21815\n", ()),
        ("Fix\n\nBug: None\n", ()),
    ],
)
def test_the_bugs_of_a_commit_are_those_its_bug_lines_name(message: str, bugs: tuple[str, ...]) -> None:
    fetch, _ = answering({"log": [commit("c" * 40, message)]})

    (found,) = commits_between(CHROMIUM_URL, "157.0.8103.27", "157.0.8103.39", fetch)

    assert found.bugs == bugs


def test_a_deleted_file_has_its_old_path_and_a_renamed_one_both() -> None:
    fetch, _ = answering(
        {
            "log": [
                commit(
                    "c" * 40,
                    "Move\n",
                    change("delete", "net/old.cc", "/dev/null"),
                    change("rename", "net/before.cc", "net/after.cc"),
                    change("add", "/dev/null", "net/new.cc"),
                )
            ]
        }
    )

    (found,) = commits_between(CHROMIUM_URL, "157.0.8103.27", "157.0.8103.39", fetch)

    assert found.changes == (
        Change("net/old.cc", "net/old.cc", "1" * 40, "2" * 40, submodule=False),
        Change("net/after.cc", "net/before.cc", "1" * 40, "2" * 40, submodule=False),
        Change("net/new.cc", "/dev/null", "1" * 40, "2" * 40, submodule=False),
    )


def test_every_page_of_the_log_counts() -> None:
    fetch, asked = answering(
        {"log": [commit("a" * 40, "First\n")], "next": "b" * 40},
        {"log": [commit("b" * 40, "Second\n")]},
    )

    commits = commits_between(CHROMIUM_URL, "157.0.8103.27", "157.0.8103.39", fetch)

    assert [found.subject for found in commits] == ["First", "Second"]
    assert urllib.parse.parse_qs(urllib.parse.urlsplit(asked[1]).query)["s"] == ["b" * 40]


def test_a_log_that_never_ends_is_an_error() -> None:
    page: dict[str, object] = {"log": [], "next": "more"}
    fetch, _ = answering(*[page] * 21)

    with pytest.raises(WebError, match="more than 20 pages"):
        commits_between(CHROMIUM_URL, "157.0.8103.27", "157.0.8103.39", fetch)


_NOT_LOGS: list[object] = [
    ["not", "a", "log"],
    {"log": "not a list"},
    {"log": ["not an object"]},
    {"log": [{"commit": "c" * 40, "tree_diff": []}]},
    {"log": [{"commit": "c" * 40, "message": "Fix\n", "tree_diff": [{"type": "modify"}]}]},
    {"log": [], "next": 2},
]


@pytest.mark.parametrize("answer", _NOT_LOGS)
def test_an_answer_that_is_not_a_log_is_an_error(answer: object) -> None:
    fetch, _ = answering(answer)

    with pytest.raises(WebError, match=r"chromium\.googlesource\.com"):
        commits_between(CHROMIUM_URL, "157.0.8103.27", "157.0.8103.39", fetch)


def test_only_a_repository_on_googlesource_is_asked() -> None:
    fetch, asked = answering({"log": []})

    with pytest.raises(WebError, match=r"not a repository on googlesource\.com"):
        commits_between("https://github.com/google/boringssl", "a" * 40, "b" * 40, fetch)
    assert asked == []


def test_the_submodules_of_a_build_are_those_of_its_gitmodules() -> None:
    urls = submodule_urls(CHROMIUM_URL, "157.0.8103.38", saved)

    assert urls == {
        "net/third_party/quiche/src": "https://quiche.googlesource.com/quiche",
        "third_party/angle": "https://chromium.googlesource.com/angle/angle",
        "third_party/boringssl/src": "https://boringssl.googlesource.com/boringssl",
        "third_party/icu": "https://chromium.googlesource.com/chromium/deps/icu",
    }


@pytest.mark.parametrize("body", [b"not base64 \xff", base64.b64encode(b"no submodules here\n")])
def test_a_gitmodules_without_submodules_is_an_error(body: bytes) -> None:
    with pytest.raises(WebError, match=r"\.gitmodules"):
        submodule_urls(CHROMIUM_URL, "157.0.8103.38", lambda _url: body)
