"""Tests of refreshing the table that a Session's Machine is drawn from, with its sources' answers of 2026-11-08 and no
network.
"""

import functools
import json
import shutil
import sys
from datetime import date
from pathlib import Path
from typing import Any

import pytest

from tests.release.saved import saved, statcounter_url
from tools.release import __main__ as release
from tools.release.machine_sources import FIREFOX_DATA_URL, refresh_table, statcounter_month
from tools.release.machines import STATISTICS_PATH, TABLE_PATH, MachineTableError, read_statistics
from tools.release.web import Fetch, WebError

SAVED_ON = date(2026, 11, 8)

# The first day on which StatCounter no longer revises October 2026, the month that the table in the repository is from.
OCTOBER_IS_FINAL = date(2026, 12, 17)

WINDOWS_AUGUST = statcounter_url("windows_version", "Windows%20Version", "2026-08")
DISPLAYS_AUGUST = statcounter_url("resolution", "Screen%20Resolution", "2026-08")
WINDOWS_OCTOBER = statcounter_url("windows_version", "Windows%20Version", "2026-10")
FIREFOX_DISPLAYS = f"{FIREFOX_DATA_URL}/resolution/index.json"
FIREFOX_MEMORY = f"{FIREFOX_DATA_URL}/ram/index.json"

AUGUST_HEAD = b'"Windows Version","Market Share Perc. (Aug 2026)"\n'


@pytest.fixture
def files(tmp_path: Path) -> tuple[Path, Path]:
    """Return copies of the statistics and the table in the repository, for a refresh to write."""
    statistics = shutil.copy(STATISTICS_PATH, tmp_path / STATISTICS_PATH.name)
    table = shutil.copy(TABLE_PATH, tmp_path / TABLE_PATH.name)
    return Path(statistics), Path(table)


def answering(changed: dict[str, bytes]) -> Fetch:
    """Return what gives the saved answers, but these for their URLs."""
    return lambda url: changed[url] if url in changed else saved(url)


def table(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


@pytest.mark.parametrize(
    ("today", "month"),
    [
        (date(2026, 11, 8), date(2026, 8, 1)),
        (date(2026, 11, 15), date(2026, 8, 1)),
        (date(2026, 11, 16), date(2026, 9, 1)),
        (date(2026, 12, 16), date(2026, 9, 1)),
        (date(2026, 12, 17), date(2026, 10, 1)),
        (date(2027, 1, 15), date(2026, 10, 1)),
        (date(2027, 1, 16), date(2026, 11, 1)),
        (date(2027, 2, 16), date(2026, 12, 1)),
    ],
)
def test_statcounters_month_is_the_newest_45_days_after_its_second_day_of_the_next(today: date, month: date) -> None:
    assert statcounter_month(today) == month


def test_a_refresh_with_the_answers_of_the_tables_periods_writes_both_files_byte_for_byte(
    files: tuple[Path, Path],
) -> None:
    statistics, table = files

    report = refresh_table(OCTOBER_IS_FINAL, saved, statistics, table)

    assert statistics.read_bytes() == STATISTICS_PATH.read_bytes()
    assert table.read_bytes() == TABLE_PATH.read_bytes()
    assert report.splitlines()[1:] == [
        "  as before: StatCounter Global Stats, Desktop Windows Version Market Share Worldwide, October 2026",
        "  as before: StatCounter Global Stats, Desktop Screen Resolution Stats Worldwide, October 2026",
        "  as before: Firefox Public Data Report, Hardware Across the Web, Display Resolution, week of 2026-10-26",
        "  as before: Firefox Public Data Report, Hardware Across the Web, Memory, week of 2026-10-26",
        "No source has anything new, so the table stays as it is.",
    ]


def test_a_refresh_takes_statcounters_newest_final_month_and_firefoxs_newest_week(files: tuple[Path, Path]) -> None:
    statistics, table_path = files

    report = refresh_table(SAVED_ON, saved, statistics, table_path)

    fetched = read_statistics(statistics)
    assert fetched.windows_versions.credit.endswith(", August 2026")
    assert fetched.css_displays.credit.endswith(", August 2026")
    assert fetched.pixel_displays.credit.endswith(", week of 2026-10-26")
    assert fetched.memory.credit.endswith(", week of 2026-10-26")
    assert dict(fetched.windows_versions.shares) == {
        "Win11": 68.6,
        "Win10": 30.15,
        "Win7": 0.95,
        "WinXP": 0.14,
        "Win8.1": 0.11,
        "Win8": 0.03,
        "WinVista": 0.02,
        "Other": 0.01,
    }
    assert table(table_path)["groups"]["platform_version"]["rows"] == [
        {"platform_version": "19.0.0", "windows": 11, "weight": round(68.6 / 98.75 * 1_000_000)},
        {"platform_version": "10.0.0", "windows": 10, "weight": round(30.15 / 98.75 * 1_000_000)},
    ]
    assert "StatCounter Global Stats, Desktop Windows Version Market Share Worldwide, August 2026, " in "\n".join(
        table(table_path)["credits"]
    )
    assert (
        "  new:       StatCounter Global Stats, Desktop Windows Version Market Share Worldwide, August 2026" in report
    )
    assert (
        "  was:       StatCounter Global Stats, Desktop Windows Version Market Share Worldwide, October 2026" in report
    )
    assert "  as before: Firefox Public Data Report, Hardware Across the Web, Memory, week of 2026-10-26" in report
    assert report.endswith(f"  git diff -- {statistics} {table_path}")


def test_a_month_that_statcounter_writes_as_sept_is_september(files: tuple[Path, Path]) -> None:
    statistics, table = files

    refresh_table(date(2026, 11, 16), saved, statistics, table)

    assert read_statistics(statistics).windows_versions.credit.endswith(", September 2026")


def test_firefoxs_shares_are_of_its_newest_week_to_three_decimals(files: tuple[Path, Path]) -> None:
    statistics, table = files

    refresh_table(SAVED_ON, saved, statistics, table)

    fetched = read_statistics(statistics)
    assert list(fetched.pixel_displays.shares.items())[:3] == [
        ("1920x1080", 51.216),
        ("1366x768", 13.482),
        ("Other", 9.571),
    ]
    # Both have shares in older weeks only.
    assert "1280x800" not in fetched.pixel_displays.shares
    assert "1GB" not in fetched.memory.shares


def test_a_refresh_keeps_the_owners_weights(files: tuple[Path, Path]) -> None:
    statistics, table_path = files
    before = table(table_path)["groups"]

    refresh_table(SAVED_ON, saved, statistics, table_path)

    after = table(table_path)["groups"]
    for group in ("network", "color_scheme", "reduced_motion", "reduced_transparency"):
        assert after[group] == before[group]
    assert "70% maximised and 30% Chrome's first window on a new profile" in after["display"]["source"]
    assert after["display"]["rows"] != before["display"]["rows"]


def test_a_revised_share_that_the_table_does_not_count_changes_the_statistics_alone(files: tuple[Path, Path]) -> None:
    statistics, table = files
    revised = saved(WINDOWS_OCTOBER).replace(b'"WinXP",0.04', b'"WinXP",0.03').replace(b'"Other",0.01', b'"Other",0.02')

    report = refresh_table(OCTOBER_IS_FINAL, answering({WINDOWS_OCTOBER: revised}), statistics, table)

    assert read_statistics(statistics).windows_versions.shares["WinXP"] == 0.03
    assert table.read_bytes() == TABLE_PATH.read_bytes()
    assert (
        "  revised:   StatCounter Global Stats, Desktop Windows Version Market Share Worldwide, October 2026" in report
    )
    assert report.endswith(f"Wrote {statistics}. The table made from it is the same as before.")


def down(url: str) -> bytes:
    raise WebError(f"{url} did not answer: HTTP Error 503: Service Unavailable")


@pytest.mark.parametrize(
    ("url", "answer", "error"),
    [
        (
            WINDOWS_AUGUST,
            b"<html><title>Just a moment...</title></html>",
            "shares of each Windows Version in August 2026",
        ),
        (
            WINDOWS_AUGUST,
            saved(statcounter_url("windows_version", "Windows%20Version", "2026-09")),
            "shares of each Windows Version in August 2026",
        ),
        (WINDOWS_AUGUST, b"\xff\xfe", "did not give a CSV"),
        (WINDOWS_AUGUST, b"", "in August 2026: nothing"),
        (WINDOWS_AUGUST, AUGUST_HEAD + b'"Win11",lots\n', "a share that is not a number"),
        (WINDOWS_AUGUST, AUGUST_HEAD + b'"Win11",68.6,1\n', "a row that is not a name and a share"),
        (WINDOWS_AUGUST, AUGUST_HEAD + b'"Win11",101\n"Win10",-1\n', "a share that is not a percentage: 101.0"),
        (WINDOWS_AUGUST, AUGUST_HEAD + b'"Win11",68.6\n"Win10",30.15\n', r"add up to 98\.750%, and not 100%"),
        (DISPLAYS_AUGUST, saved(WINDOWS_AUGUST), "shares of each Screen Resolution in August 2026"),
        (FIREFOX_DISPLAYS, b"not json", "did not give JSON"),
        (FIREFOX_DISPLAYS, b'{"data": {}}', "gave no populations"),
        (FIREFOX_DISPLAYS, b'{"data": {"populations": {}}}', "gave no week"),
        (FIREFOX_DISPLAYS, b'{"data": {"populations": {"1920x1080": 51}}}', "no list of weeks for 1920x1080"),
        (
            FIREFOX_DISPLAYS,
            b'{"data": {"populations": {"1920x1080": [{"x": "this week", "y": 100}]}}}',
            "a point that is not a week and a share",
        ),
        (
            FIREFOX_DISPLAYS,
            b'{"data": {"populations": {"1920x1080": [{"x": "2026-10-26", "y": true}]}}}',
            "a point that is not a week and a share",
        ),
        (
            FIREFOX_MEMORY,
            b'{"data": {"populations": {"8GB": [{"x": "2026-10-26", "y": 29.453}]}}}',
            r"add up to 29\.453%",
        ),
    ],
)
def test_an_answer_that_is_not_what_a_refresh_reads_changes_neither_file(
    files: tuple[Path, Path], url: str, answer: bytes, error: str
) -> None:
    statistics, table = files

    with pytest.raises(WebError, match=error) as raised:
        refresh_table(SAVED_ON, answering({url: answer}), statistics, table)

    assert str(raised.value).startswith(url)
    assert statistics.read_bytes() == STATISTICS_PATH.read_bytes()
    assert table.read_bytes() == TABLE_PATH.read_bytes()


def test_a_source_that_does_not_answer_changes_neither_file(files: tuple[Path, Path]) -> None:
    statistics, table = files

    def memory_down(url: str) -> bytes:
        return down(url) if url == FIREFOX_MEMORY else saved(url)

    with pytest.raises(WebError, match=f"^{FIREFOX_MEMORY} did not answer"):
        refresh_table(SAVED_ON, memory_down, statistics, table)

    assert statistics.read_bytes() == STATISTICS_PATH.read_bytes()
    assert table.read_bytes() == TABLE_PATH.read_bytes()


def test_statistics_that_cannot_make_the_table_change_neither_file(files: tuple[Path, Path]) -> None:
    statistics, table = files
    no_windows_10 = AUGUST_HEAD + b'"Win11",99.98\n"Other",0.02\n'

    with pytest.raises(MachineTableError, match="Win11 and Win10"):
        refresh_table(SAVED_ON, answering({WINDOWS_AUGUST: no_windows_10}), statistics, table)

    assert statistics.read_bytes() == STATISTICS_PATH.read_bytes()
    assert table.read_bytes() == TABLE_PATH.read_bytes()


def run_machines(monkeypatch: pytest.MonkeyPatch, *args: str) -> int:
    monkeypatch.setattr(sys, "argv", ["python -m tools.release", "machines", *args])
    return release.main()


def test_the_routine_says_why_it_did_not_refresh_the_table(
    files: tuple[Path, Path], monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    statistics, table = files
    monkeypatch.setattr(release, "refresh_table", functools.partial(refresh_table, fetch=down))

    assert run_machines(monkeypatch, "--statistics", str(statistics), "--table", str(table)) == 1

    assert capsys.readouterr().err.startswith(
        "error: the table stays as it was, because https://gs.statcounter.com/chart.php?"
    )
    assert table.read_bytes() == TABLE_PATH.read_bytes()


def test_the_routine_writes_the_table_from_the_saved_statistics_without_the_network(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    table = tmp_path / "table.json"
    monkeypatch.setattr(release, "refresh_table", down)

    assert run_machines(monkeypatch, "--saved", "--table", str(table)) == 0

    assert table.read_bytes() == TABLE_PATH.read_bytes()
