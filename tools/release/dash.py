"""What Chromium Dash says of a Chrome Release's schedule: when it becomes stable, which starts its run of the routine."""

from dataclasses import dataclass
from datetime import date, datetime
from typing import Any, cast

from tools.release.web import Fetch, WebError, fetch_from_the_network, json_answer

__all__ = ["DASH_URL", "Milestone", "milestone"]

DASH_URL = "https://chromiumdash.appspot.com"


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


def milestone(release: str, fetch: Fetch = fetch_from_the_network) -> Milestone:
    """Return the schedule of a Chrome Release.

    Raises:
        WebError: Chromium Dash did not answer, or its answer cannot be read.
    """
    url = f"{DASH_URL}/fetch_milestone_schedule?mstone={release}"
    answer = json_answer(fetch(url), url)
    schedules: Any = cast(dict[str, Any], answer).get("mstones") if isinstance(answer, dict) else None
    schedule: Any = cast(list[Any], schedules)[0] if isinstance(schedules, list) and schedules else None
    if not isinstance(schedule, dict):
        raise WebError(f"{url} gave no schedule of milestone {release}")
    schedule = cast(dict[str, Any], schedule)
    if str(schedule.get("mstone")) != release:
        raise WebError(f"{url} gave no schedule of milestone {release}")
    return Milestone(release, _date(schedule, "stable_date", url), _date(schedule, "early_stable", url))


def _date(schedule: dict[str, Any], key: str, url: str) -> date:
    value = schedule.get(key)
    try:
        return datetime.fromisoformat(cast(str, value)).date()
    except (TypeError, ValueError) as error:
        raise WebError(f"{url} gave no date as {key}: {value!r}") from error
