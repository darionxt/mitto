"""Tests of which build of each Chrome Release a recording uses, from Google's version history, with no network."""

import json
import urllib.parse
from datetime import date

import pytest

from tests.release.saved import saved
from tools.release.version_history import RELEASES_URL, StableBuild, newest_pinnable_builds, stable_builds
from tools.release.web import Fetch, WebError


def answering(*pages: object) -> tuple[Fetch, list[str]]:
    """Return what gives these pages in turn, and the URLs it was asked for."""
    asked: list[str] = []

    def fetch(url: str) -> bytes:
        asked.append(url)
        return json.dumps(pages[len(asked) - 1]).encode()

    return fetch, asked


def test_the_newest_pinnable_builds_are_those_of_the_day() -> None:
    # On 2026-11-07, 156.0.8078.82 and 158.0.8127.18 went to 0.5% of installations, and could not be pinned.
    assert newest_pinnable_builds(["156", "157", "158"], saved) == {
        "156": "156.0.8078.80",
        "157": "157.0.8103.39",
        "158": None,
    }


def test_the_question_asks_for_the_pinnable_builds_of_the_chrome_releases_only() -> None:
    fetch, asked = answering({"releases": []})

    newest_pinnable_builds(["157", "155", "156"], fetch)

    (url,) = asked
    assert url.startswith(f"{RELEASES_URL}?")
    assert urllib.parse.parse_qs(urllib.parse.urlsplit(url).query) == {
        "filter": ["version>=155,version<158,pinnable=true"]
    }


def test_a_build_that_cannot_be_pinned_is_never_the_one() -> None:
    fetch, _ = answering(
        {
            "releases": [
                {"version": "157.0.8103.40", "pinnable": False},
                {"version": "157.0.8103.39", "pinnable": True},
                {"version": "157.0.8103.41"},
            ]
        }
    )

    assert newest_pinnable_builds(["157"], fetch) == {"157": "157.0.8103.39"}


def test_the_newest_build_is_the_highest_version_and_not_the_first_or_the_highest_text() -> None:
    fetch, _ = answering(
        {
            "releases": [
                {"version": "154.0.8037.99", "pinnable": True},
                {"version": "154.0.8037.100", "pinnable": True},
                {"version": "154.0.8037.98", "pinnable": True},
            ]
        }
    )

    assert newest_pinnable_builds(["154"], fetch) == {"154": "154.0.8037.100"}


def test_a_build_of_another_chrome_release_counts_for_none_of_those_asked_for() -> None:
    fetch, _ = answering({"releases": [{"version": "156.0.8078.80", "pinnable": True}]})

    assert newest_pinnable_builds(["155", "157"], fetch) == {"155": None, "157": None}


def test_every_page_of_the_answer_counts() -> None:
    fetch, asked = answering(
        {"releases": [{"version": "157.0.8103.27", "pinnable": True}], "nextPageToken": "second page"},
        {"releases": [{"version": "157.0.8103.39", "pinnable": True}], "nextPageToken": ""},
    )

    assert newest_pinnable_builds(["157"], fetch) == {"157": "157.0.8103.39"}
    assert urllib.parse.parse_qs(urllib.parse.urlsplit(asked[1]).query)["pageToken"] == ["second page"]


def test_an_answer_that_never_ends_is_an_error() -> None:
    page: dict[str, object] = {"releases": [], "nextPageToken": "more"}
    fetch, _ = answering(*[page] * 21)

    with pytest.raises(WebError, match="more than 20 pages"):
        newest_pinnable_builds(["157"], fetch)


@pytest.mark.parametrize(
    "answer",
    [
        "not json",
        ["157.0.8103.39"],
        {"releases": "157.0.8103.39"},
        {"releases": ["157.0.8103.39"]},
        {"releases": [{"pinnable": True}]},
        {"releases": [{"version": "157.0.8103", "pinnable": True}]},
        {"releases": [{"version": "157.0.8103.x", "pinnable": True}]},
        {"releases": [], "nextPageToken": 2},
    ],
)
def test_an_answer_that_is_not_a_list_of_releases_is_an_error(answer: object) -> None:
    fetch: Fetch = (lambda _url: b"not json") if answer == "not json" else answering(answer)[0]

    with pytest.raises(WebError, match=r"versionhistory\.googleapis\.com"):
        newest_pinnable_builds(["157"], fetch)


def test_the_stable_builds_of_a_chrome_release_are_those_of_the_day() -> None:
    builds = stable_builds("157", saved)

    assert [(build.version, build.pinnable) for build in builds] == [
        ("157.0.8103.15", False),
        ("157.0.8103.26", False),
        ("157.0.8103.27", False),
        ("157.0.8103.38", False),
        ("157.0.8103.39", True),
        ("157.0.8103.40", False),
        ("157.0.8103.50", False),
        ("157.0.8103.51", True),
    ]
    assert builds[4] == StableBuild("157.0.8103.39", date(2026, 11, 3), 1.0, pinnable=True)


def test_a_stable_build_takes_its_first_start_its_largest_share_and_any_pin() -> None:
    fetch, asked = answering(
        {
            "releases": [
                {"version": "157.0.8103.39", "fraction": 0.5, "serving": {"startTime": "2026-11-05T21:11:00Z"}},
                {
                    "version": "157.0.8103.39",
                    "fraction": 0.1,
                    "pinnable": True,
                    "serving": {"startTime": "2026-11-03T21:11:00.5Z"},
                },
                {"version": "157.0.8103.100", "fraction": 0.005, "serving": {"startTime": "2026-11-09T20:47:31Z"}},
                {"version": "158.0.8127.18", "fraction": 0.005, "serving": {"startTime": "2026-11-09T20:47:31Z"}},
            ]
        }
    )

    assert stable_builds("157", fetch) == [
        StableBuild("157.0.8103.39", date(2026, 11, 3), 0.5, pinnable=True),
        StableBuild("157.0.8103.100", date(2026, 11, 9), 0.005, pinnable=False),
    ]
    assert urllib.parse.parse_qs(urllib.parse.urlsplit(asked[0]).query) == {"filter": ["version>=157,version<158"]}


@pytest.mark.parametrize(
    "release",
    [
        {"version": "157.0.8103.39", "fraction": 1},
        {"version": "157.0.8103.39", "fraction": 1, "serving": {"startTime": "yesterday"}},
        {"version": "157.0.8103.39", "fraction": "all", "serving": {"startTime": "2026-11-03T21:11:00Z"}},
        {"fraction": 1, "serving": {"startTime": "2026-11-03T21:11:00Z"}},
    ],
)
def test_a_stable_build_that_cannot_be_read_is_an_error(release: dict[str, object]) -> None:
    fetch, _ = answering({"releases": [release]})

    with pytest.raises(WebError, match=r"versionhistory\.googleapis\.com"):
        stable_builds("157", fetch)
