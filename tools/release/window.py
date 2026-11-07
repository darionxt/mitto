"""The Supported Window of the next run of the release routine, and what to record for it. See docs/releasing.md."""

import re
from collections.abc import Iterable
from datetime import date
from pathlib import Path

from tools.parity import BASELINES_DIRECTORY
from tools.release.dash import Fetch, fetch_from_the_network, milestone, newest_builds

__all__ = ["WINDOW_SIZE", "builds_report", "next_window", "recorded_build"]

WINDOW_SIZE = 3
"""How many Chrome Releases the Supported Window holds: the Engine Release and the two before it."""

_NOTE = "chrome.txt"
_RECORDED_BUILD = re.compile(r"^Chrome: (\S+)$", re.MULTILINE)


def next_window(releases: Iterable[str]) -> list[str]:
    """Return the Supported Window after the one of these Chrome Releases, oldest first.

    Its Engine Release is the milestone after the newest of them.
    """
    engine = max(int(release) for release in releases) + 1
    return [str(release) for release in range(engine - WINDOW_SIZE + 1, engine + 1)]


def recorded_build(release: str, baselines_directory: Path = BASELINES_DIRECTORY) -> str | None:
    """Return the build that recorded the Baselines of a Chrome Release, or None if it has none."""
    note = baselines_directory / release / _NOTE
    if not note.is_file():
        return None
    found = _RECORDED_BUILD.search(note.read_text(encoding="utf-8"))
    return found[1] if found else None


def builds_report(
    window: list[str],
    today: date,
    fetch: Fetch = fetch_from_the_network,
    baselines_directory: Path = BASELINES_DIRECTORY,
) -> str:
    """Return what Chromium Dash says of a Supported Window, for whoever runs the release routine.

    That is when its Engine Release becomes stable, and for each Chrome Release, the newest Windows stable build beside
    the build of its Baselines.

    Raises:
        DashError: Chromium Dash did not answer, or its answer cannot be read.
    """
    engine = window[-1]
    schedule = milestone(engine, fetch)
    builds = newest_builds(window, fetch)
    names = f"{', '.join(window[:-1])} and {engine}"
    if today < schedule.stable_date:
        days = (schedule.stable_date - today).days
        start = f"Chrome {engine} becomes stable on {schedule.stable_date}, in {days} days. Its run starts then."
    else:
        start = f"Chrome {engine} became stable on {schedule.stable_date}, so its run can start."
    rows = [("Chrome Release", "Newest Windows stable build", "Build of the Baselines")]
    rows += [(release, builds[release], recorded_build(release, baselines_directory) or "none") for release in window]
    widths = [max(len(row[column]) for row in rows) for column in range(len(rows[0]))]
    table = ["  ".join(cell.ljust(width) for cell, width in zip(row, widths, strict=True)).rstrip() for row in rows]
    return "\n".join(
        [
            f"Next Supported Window: Chrome {names}, with {engine} as the Engine Release.",
            f"{start} Its early stable date is {schedule.early_stable}.",
            "",
            *table,
            "",
            "Record the build that Google's update server gives. docs/baselines.md says how to ask it.",
        ]
    )
