"""What Chromium Dash says of Chrome's stable milestones: the newest Windows build of each, and when one becomes stable.

Chromium Dash only helps find a build. The build a recording uses is the one Google's update server gives, as
docs/baselines.md says.
"""

import json
import urllib.request
from collections.abc import Callable, Iterable
from dataclasses import dataclass
from datetime import date, datetime
from typing import Any, cast

__all__ = [
    "DASH_URL",
    "DashError",
    "Fetch",
    "Milestone",
    "fetch_from_the_network",
    "milestone",
    "newest_builds",
]

DASH_URL = "https://chromiumdash.appspot.com"

_TIMEOUT_SECONDS = 30.0
_PLATFORM = "Windows"
_CHANNEL = "Stable"

type Fetch = Callable[[str], bytes]
"""What returns the body of the answer to a URL, so that tests can give saved answers."""


class DashError(Exception):
    """Chromium Dash did not answer, or its answer is not what this module reads."""


@dataclass(frozen=True, slots=True)
class Milestone:
    """The schedule of one Chrome Release.

    Attributes:
        release: The Chrome Release, such as "158".
        stable_date: When it becomes stable for every user, which starts its run of the release routine.
        early_stable: When it reaches a small share of users first.
    """

    release: str
    stable_date: date
    early_stable: date


def fetch_from_the_network(url: str) -> bytes:
    """Return the body of the answer to a URL.

    Raises:
        DashError: The request failed.
    """
    try:
        with urllib.request.urlopen(url, timeout=_TIMEOUT_SECONDS) as answer:
            return cast(bytes, answer.read())
    except OSError as error:
        raise DashError(f"{url} did not answer: {error}") from error


def newest_builds(releases: Iterable[str], fetch: Fetch = fetch_from_the_network, *, count: int = 50) -> dict[str, str]:
    """Return the newest Windows stable build of each Chrome Release, by Chrome Release.

    Args:
        releases: The Chrome Releases, such as ["156", "157", "158"].
        fetch: What asks Chromium Dash.
        count: How many of the newest Windows stable builds to ask for. They hold the builds of every milestone that is
            still updated, newest first.

    Raises:
        DashError: Chromium Dash did not answer, its answer cannot be read, or those builds hold none of a Chrome Release.
    """
    url = f"{DASH_URL}/fetch_releases?channel={_CHANNEL}&platform={_PLATFORM}&num={count}"
    builds: dict[str, tuple[int, ...]] = {}
    for entry in _json_list(fetch(url), url):
        version = entry.get("version")
        release = entry.get("milestone")
        if not isinstance(version, str) or not isinstance(release, int):
            raise DashError(f"{url} gave a build without a version and a milestone: {entry}")
        parts = _version_parts(version, url)
        if parts[0] != release:
            raise DashError(f"{url} gave {version} as a build of milestone {release}")
        builds[str(release)] = max(builds.get(str(release), parts), parts)
    newest: dict[str, str] = {}
    for release in releases:
        if release not in builds:
            raise DashError(f"the newest {count} Windows stable builds in Chromium Dash hold none of Chrome {release}")
        newest[release] = ".".join(map(str, builds[release]))
    return newest


def milestone(release: str, fetch: Fetch = fetch_from_the_network) -> Milestone:
    """Return the schedule of a Chrome Release.

    Raises:
        DashError: Chromium Dash did not answer, or its answer cannot be read.
    """
    url = f"{DASH_URL}/fetch_milestone_schedule?mstone={release}"
    answer = _json(fetch(url), url)
    schedules: Any = cast(dict[str, Any], answer).get("mstones") if isinstance(answer, dict) else None
    schedule: Any = cast(list[Any], schedules)[0] if isinstance(schedules, list) and schedules else None
    if not isinstance(schedule, dict):
        raise DashError(f"{url} gave no schedule of milestone {release}")
    schedule = cast(dict[str, Any], schedule)
    if str(schedule.get("mstone")) != release:
        raise DashError(f"{url} gave no schedule of milestone {release}")
    return Milestone(release, _date(schedule, "stable_date", url), _date(schedule, "early_stable", url))


def _json(body: bytes, url: str) -> Any:
    try:
        return json.loads(body)
    except ValueError as error:
        raise DashError(f"{url} did not give JSON: {error}") from error


def _json_list(body: bytes, url: str) -> list[dict[str, Any]]:
    answer = _json(body, url)
    if not isinstance(answer, list) or not all(isinstance(entry, dict) for entry in cast(list[Any], answer)):
        raise DashError(f"{url} did not give a list of builds")
    return cast(list[dict[str, Any]], answer)


def _version_parts(version: str, url: str) -> tuple[int, ...]:
    parts = version.split(".")
    if len(parts) != 4 or not all(part.isdigit() for part in parts):
        raise DashError(f"{url} gave a version that is not four numbers: {version}")
    return tuple(int(part) for part in parts)


def _date(schedule: dict[str, Any], key: str, url: str) -> date:
    value = schedule.get(key)
    try:
        return datetime.fromisoformat(cast(str, value)).date()
    except (TypeError, ValueError) as error:
        raise DashError(f"{url} gave no date as {key}: {value!r}") from error
