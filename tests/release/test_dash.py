"""Tests of what the release routine reads from Chromium Dash, and of the report on the next Supported Window.

They use answers saved on 2026-11-07, and no network.
"""

from datetime import date
from pathlib import Path

import pytest

from mitto._persona import WINDOWS_PERSONAS
from tests.release.saved import saved
from tools.release.dash import Milestone, milestone
from tools.release.web import WebError, fetch_from_the_network
from tools.release.window import builds_report, next_window, recorded_build

SAVED_ON = date(2026, 11, 7)


def test_the_schedule_gives_the_stable_date_and_the_early_stable_date() -> None:
    assert milestone("158", saved) == Milestone("158", stable_date=date(2026, 11, 17), early_stable=date(2026, 11, 4))


@pytest.mark.parametrize(
    "answer",
    [
        b"not json",
        b"[]",
        b'{"mstones": []}',
        b'{"mstones": [{"mstone": 159, "stable_date": "2026-12-08T00:00:00", "early_stable": "2026-11-24T00:00:00"}]}',
        b'{"mstones": [{"mstone": 158, "early_stable": "2026-11-04T00:00:00"}]}',
        b'{"mstones": [{"mstone": 158, "stable_date": "soon", "early_stable": "2026-11-04T00:00:00"}]}',
    ],
)
def test_an_answer_that_is_not_the_schedule_of_the_milestone_is_an_error(answer: bytes) -> None:
    with pytest.raises(WebError, match=r"fetch_milestone_schedule\?mstone=158"):
        milestone("158", lambda _url: answer)


def test_a_request_that_fails_is_an_error() -> None:
    # Nothing listens on port 1 of the loopback address, so this goes nowhere.
    with pytest.raises(WebError, match="did not answer"):
        fetch_from_the_network("http://127.0.0.1:1/")


@pytest.mark.parametrize(
    ("releases", "window"),
    [
        (["155", "156", "157"], ["156", "157", "158"]),
        (["157", "155", "156"], ["156", "157", "158"]),
        (["156", "157", "158"], ["157", "158", "159"]),
    ],
)
def test_the_next_window_ends_at_the_milestone_after_the_newest(releases: list[str], window: list[str]) -> None:
    assert next_window(releases) == window


def write_note(baselines: Path, release: str, build: str) -> None:
    directory = baselines / release
    directory.mkdir(parents=True)
    (directory / "chrome.txt").write_text(f"Recorded: 2026-11-07\nProduct: Google Chrome\nChrome: {build}\nFlags: -\n")


def test_the_recorded_build_is_the_one_in_the_note_of_the_baselines(tmp_path: Path) -> None:
    write_note(tmp_path, "157", "157.0.8103.39")

    assert recorded_build("157", tmp_path) == "157.0.8103.39"
    assert recorded_build("158", tmp_path) is None


@pytest.mark.parametrize("release", WINDOWS_PERSONAS)
def test_the_recorded_build_of_each_chrome_release_in_the_repository_is_one_of_its_builds(release: str) -> None:
    build = recorded_build(release)

    assert build is not None
    assert build.startswith(f"{release}.0.")


def test_the_report_says_when_the_run_starts_and_which_builds_differ(tmp_path: Path) -> None:
    write_note(tmp_path, "156", "156.0.8078.74")
    write_note(tmp_path, "157", "157.0.8103.39")

    report = builds_report(["156", "157", "158"], SAVED_ON, saved, tmp_path)

    assert report == "\n".join(
        [
            "Next Supported Window: Chrome 156, 157 and 158, with 158 as the Engine Release.",
            "Chrome 158 becomes stable on 2026-11-17, in 10 days. Its run starts then. "
            "Its early stable date is 2026-11-04.",
            "",
            "Chrome Release  Newest pinnable build  Build of the Baselines",
            "156             156.0.8078.80          156.0.8078.74",
            "157             157.0.8103.39          157.0.8103.39",
            "158             none yet               none",
            "",
            "A recording takes the newest pinnable build, which Google's update server gives for a Chrome Release.",
            "docs/baselines.md says how to ask it.",
        ]
    )


@pytest.mark.parametrize("today", [date(2026, 11, 17), date(2026, 11, 20)])
def test_the_report_says_that_the_run_can_start_from_the_stable_date(tmp_path: Path, today: date) -> None:
    report = builds_report(["156", "157", "158"], today, saved, tmp_path)

    assert "Chrome 158 became stable on 2026-11-17, so its run can start." in report
