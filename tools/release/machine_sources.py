"""Fetching the public statistics that the table of Machines is made from again, and writing the table from them.

StatCounter has a month on the second of the next and revises it for 45 days, so a refresh takes its newest month that
it no longer revises, through the CSV link that its pages use for their download. The Firefox Public Data Report has a
new week each week, at stable JSON addresses, so a refresh takes its newest week. The time zones that Chrome on Windows
can give come from the ICU that the Engine Release builds with, through Chromium's Gitiles servers. See
docs/releasing.md.
"""

import base64
import binascii
import csv
import io
import re
import urllib.parse
from collections.abc import Mapping
from dataclasses import dataclass
from datetime import date, timedelta
from pathlib import Path
from typing import Any, cast

from tools.release.gitiles import CHROMIUM_URL
from tools.release.machines import (
    STATISTICS_PATH,
    TABLE_PATH,
    Source,
    Statistics,
    dumps_statistics,
    dumps_table,
    read_statistics,
    table_data,
)
from tools.release.time_zones import TIME_ZONES_PATH, TimeZonesError, dumps_time_zones, windows_time_zones
from tools.release.web import Fetch, WebError, fetch_from_the_network, json_answer

__all__ = [
    "FIREFOX_DATA_URL",
    "ICU_URL",
    "STATCOUNTER_CHART_URL",
    "fetch_statistics",
    "fetch_time_zones",
    "refresh_table",
    "statcounter_month",
]

STATCOUNTER_CHART_URL = "https://gs.statcounter.com/chart.php"
FIREFOX_DATA_URL = "https://data.firefox.com/datasets/desktop/hardware/default"
ICU_URL = "https://chromium.googlesource.com/chromium/deps/icu"

# Where Chromium's DEPS pins the ICU that it builds with, and where ICU keeps its table of Windows' time zones.
_ICU_PIN = re.compile(r"/chromium/deps/icu\.git' \+ '@' \+ '([0-9a-f]{40})'")
_WINDOWS_ZONES = "source/data/misc/windowsZones.txt"

_REPOSITORY = Path(__file__).parents[2]

_STATCOUNTER = "StatCounter Global Stats"
_FIREFOX = "Firefox Public Data Report, Hardware Across the Web"
_FIREFOX_PAGE = "https://data.firefox.com/dashboard/hardware"

# StatCounter has a month on this day of the next, and revises it for this long.
_STATCOUNTER_PUBLISHED_DAY = 2
_STATCOUNTER_REVISION = timedelta(days=45)

_MONTHS = (
    "January",
    "February",
    "March",
    "April",
    "May",
    "June",
    "July",
    "August",
    "September",
    "October",
    "November",
    "December",
)

# StatCounter's CSV names its period in the head of its second column, such as "Market Share Perc. (Sept 2026)".
_STATCOUNTER_PERIOD = re.compile(r"Market Share Perc\. \(([A-Za-z]+) (\d{4})\)")

# Firefox gives its shares to 16 digits. Three decimals, finer than StatCounter's two, keep the statistics readable.
_FIREFOX_DECIMALS = 3

# The shares of one source add up to 100%, less what their rounding loses.
_LEEWAY_PERCENT = 0.5


@dataclass(frozen=True, slots=True)
class _StatCounterStat:
    """One of StatCounter's statistics: its key and name in the CSV link, and the title and address of its page."""

    key: str
    name: str
    title: str
    page: str


_WINDOWS_VERSIONS = _StatCounterStat(
    "windows_version",
    "Windows Version",
    "Desktop Windows Version Market Share Worldwide",
    "https://gs.statcounter.com/windows-version-market-share/desktop/worldwide",
)
_CSS_DISPLAYS = _StatCounterStat(
    "resolution",
    "Screen Resolution",
    "Desktop Screen Resolution Stats Worldwide",
    "https://gs.statcounter.com/screen-resolution-stats/desktop/worldwide",
)


def statcounter_month(today: date) -> date:
    """Return the first day of StatCounter's newest month that it no longer revises on `today`."""
    month = today.replace(day=1)
    while True:
        month = (month - timedelta(days=1)).replace(day=1)
        published = (month + timedelta(days=31)).replace(day=_STATCOUNTER_PUBLISHED_DAY)
        if published + _STATCOUNTER_REVISION <= today:
            return month


def fetch_statistics(today: date, fetch: Fetch = fetch_from_the_network) -> Statistics:
    """Return what the sources say on `today`: StatCounter's newest month that it no longer revises, and Firefox's
    newest week.

    Raises:
        WebError: A source did not answer, or its answer is not what this reads.
    """
    month = statcounter_month(today)
    return Statistics(
        windows_versions=_statcounter(_WINDOWS_VERSIONS, month, fetch),
        css_displays=_statcounter(_CSS_DISPLAYS, month, fetch),
        pixel_displays=_firefox("resolution", "Display Resolution", fetch),
        memory=_firefox("ram", "Memory", fetch),
        processor_cores=_firefox("cpuCores", "CPU Cores", fetch),
    )


def fetch_time_zones(build: str, fetch: Fetch = fetch_from_the_network) -> str:
    """Return the list of time zones that Chrome on Windows can give, as time_zones.json holds it, from the windowsZones
    table of the ICU that a build of Chromium pins in its DEPS.

    Raises:
        WebError: A server did not answer, or its answer is not what this reads.
    """
    deps_url = f"{CHROMIUM_URL}/+/{build}/DEPS?format=TEXT"
    pin = _ICU_PIN.search(_gitiles_file(fetch, deps_url))
    if pin is None:
        raise WebError(f"{deps_url} pins no ICU")
    table_url = f"{ICU_URL}/+/{pin[1]}/{_WINDOWS_ZONES}?format=TEXT"
    try:
        zones = windows_time_zones(_gitiles_file(fetch, table_url))
    except TimeZonesError as error:
        raise WebError(f"{table_url} did not give ICU's table of Windows' time zones: {error}") from error
    source = (
        f"The first time zone of each region in the windowsZones table of ICU, from Unicode's CLDR, at {pin[1][:12]},"
        f" which Chromium {build} builds with: https://github.com/unicode-org/cldr, under the Unicode License v3"
    )
    return dumps_time_zones(source, zones)


def refresh_table(
    today: date,
    build: str,
    fetch: Fetch = fetch_from_the_network,
    statistics_path: Path = STATISTICS_PATH,
    table_path: Path = TABLE_PATH,
    time_zones_path: Path = TIME_ZONES_PATH,
) -> str:
    """Fetch the statistics again, write them and the table made from them, write the time zones of the Engine Release's
    build of Chromium, and return what changed.

    No file changes unless every source answered and the table can be made from what they said. The owner's weights are
    not fetched, so they stay as the table had them.

    Raises:
        WebError: A source did not answer, or its answer is not what this reads.
        MachineTableError: The statistics cannot make the table.
    """
    before = read_statistics(statistics_path)
    after = fetch_statistics(today, fetch)
    time_zones = fetch_time_zones(build, fetch)
    table = dumps_table(table_data(after))
    table_changed = table_path.read_text(encoding="utf-8") != table
    time_zones_changed = time_zones_path.read_text(encoding="utf-8") != time_zones
    statistics_path.write_text(dumps_statistics(after), encoding="utf-8", newline="\n")
    table_path.write_text(table, encoding="utf-8", newline="\n")
    time_zones_path.write_text(time_zones, encoding="utf-8", newline="\n")

    lines = ["What each source says now:"]
    for old, new in zip(_sources(before), _sources(after), strict=True):
        if old.credit != new.credit:
            lines += [f"  new:       {new.credit}", f"  was:       {old.credit}"]
        elif old.shares != new.shares:
            lines.append(f"  revised:   {new.credit}")
        else:
            lines.append(f"  as before: {new.credit}")
    if table_changed:
        lines += [
            f"Wrote {_shown(statistics_path)} and {_shown(table_path)}. Read their diff before committing them:",
            f"  git diff -- {_shown(statistics_path)} {_shown(table_path)}",
        ]
    elif before != after:
        lines.append(f"Wrote {_shown(statistics_path)}. The table made from it is the same as before.")
    else:
        lines.append("No source has anything new, so the table stays as it is.")
    if time_zones_changed:
        lines.append(f"Wrote {_shown(time_zones_path)}, the time zones of Chromium {build}. Read its diff too.")
    else:
        lines.append(f"The time zones of Chromium {build} are those of {_shown(time_zones_path)} already.")
    return "\n".join(lines)


def _sources(statistics: Statistics) -> tuple[Source, ...]:
    return (
        statistics.windows_versions,
        statistics.css_displays,
        statistics.pixel_displays,
        statistics.memory,
        statistics.processor_cores,
    )


def _gitiles_file(fetch: Fetch, url: str) -> str:
    """Return the text of a file that a Gitiles server gives in base64, as it does with format=TEXT."""
    try:
        return base64.b64decode(fetch(url), validate=False).decode("utf-8-sig")
    except (binascii.Error, UnicodeDecodeError) as error:
        raise WebError(f"{url} did not give a file: {error}") from error


def _shown(path: Path) -> str:
    """Return a path from the root of the repository if it is in it, so that it reads as git names it."""
    return str(path.relative_to(_REPOSITORY)) if path.is_relative_to(_REPOSITORY) else str(path)


def _statcounter_csv_url(stat: _StatCounterStat, month: date) -> str:
    """Return the link of the CSV that StatCounter's page of a statistic downloads, for one month."""
    query = {
        "device": "Desktop",
        "device_hidden": "desktop",
        "statType_hidden": stat.key,
        "region_hidden": "ww",
        "granularity": "monthly",
        "statType": stat.name,
        "region": "Worldwide",
        "fromInt": f"{month:%Y%m}",
        "toInt": f"{month:%Y%m}",
        "fromMonthYear": f"{month:%Y-%m}",
        "toMonthYear": f"{month:%Y-%m}",
        "csv": "1",
    }
    return f"{STATCOUNTER_CHART_URL}?{urllib.parse.urlencode(query, quote_via=urllib.parse.quote)}"


def _statcounter(stat: _StatCounterStat, month: date, fetch: Fetch) -> Source:
    """Return what StatCounter says of a statistic for one month."""
    url = _statcounter_csv_url(stat, month)
    period = f"{_MONTHS[month.month - 1]} {month.year}"
    try:
        rows = list(csv.reader(io.StringIO(fetch(url).decode("utf-8"))))
    except (UnicodeDecodeError, csv.Error) as error:
        raise WebError(f"{url} did not give a CSV: {error}") from error
    if not rows or len(rows[0]) != 2 or rows[0][0] != stat.name or not _names_month(rows[0][1], month):
        head = rows[0] if rows else "nothing"
        raise WebError(f"{url} did not give StatCounter's shares of each {stat.name} in {period}: {head}")
    shares: dict[str, float] = {}
    for row in rows[1:]:
        if len(row) != 2 or not row[0] or row[0] in shares:
            raise WebError(f"{url} gave a row that is not a name and a share: {row}")
        try:
            share = float(row[1])
        except ValueError:
            raise WebError(f"{url} gave a share that is not a number: {row}") from None
        shares[row[0]] = _percentage(share, url)
    _check_total(shares, url)
    return Source(f"{_STATCOUNTER}, {stat.title}, {period}", stat.page, shares)


def _names_month(head: str, month: date) -> bool:
    """Return whether the head of StatCounter's column names this month, by an abbreviation such as "Sep" or "Sept"."""
    found = _STATCOUNTER_PERIOD.fullmatch(head)
    if found is None:
        return False
    name, year = found[1].lower(), int(found[2])
    return len(name) >= 3 and _MONTHS[month.month - 1].lower().startswith(name) and year == month.year


def _firefox(metric: str, title: str, fetch: Fetch) -> Source:
    """Return what the Firefox Public Data Report says of a metric for its newest week."""
    url = f"{FIREFOX_DATA_URL}/{metric}/index.json"
    by_week = _firefox_weeks(json_answer(fetch(url), url), url)
    if not by_week:
        raise WebError(f"{url} gave no week")
    week = max(by_week)
    shares = {name: round(share, _FIREFOX_DECIMALS) for name, share in by_week[week].items()}
    _check_total(shares, url)
    largest_first = dict(sorted(shares.items(), key=lambda item: -item[1]))
    return Source(f"{_FIREFOX}, {title}, week of {week.isoformat()}", _FIREFOX_PAGE, largest_first)


def _firefox_weeks(answer: Any, url: str) -> dict[date, dict[str, float]]:
    """Return the share of each name in each week of an answer of the Firefox Public Data Report.

    Its shares are {"data": {"populations": {name: [{"x": week, "y": share}, ...]}}}, and a name that has no share in a
    week counts for nothing in it.
    """
    data: Any = cast(dict[str, Any], answer).get("data") if isinstance(answer, dict) else None
    populations: Any = cast(dict[str, Any], data).get("populations") if isinstance(data, dict) else None
    if not isinstance(populations, dict):
        raise WebError(f"{url} gave no populations")
    by_week: dict[date, dict[str, float]] = {}
    for name, points in cast(dict[str, Any], populations).items():
        if not isinstance(points, list):
            raise WebError(f"{url} gave no list of weeks for {name}")
        for point in cast(list[Any], points):
            week, share = _firefox_point(point, url)
            by_week.setdefault(week, {})[name] = share
    return by_week


def _firefox_point(point: Any, url: str) -> tuple[date, float]:
    if isinstance(point, dict):
        fields = cast(dict[str, Any], point)
        week, share = fields.get("x"), fields.get("y")
        if isinstance(week, str) and isinstance(share, int | float) and not isinstance(share, bool):
            try:
                day = date.fromisoformat(week)
            except ValueError:
                pass
            else:
                return day, _percentage(share, url)
    raise WebError(f"{url} gave a point that is not a week and a share: {point!r}")


def _percentage(share: float, url: str) -> float:
    if not 0 <= share <= 100:
        raise WebError(f"{url} gave a share that is not a percentage: {share!r}")
    return share


def _check_total(shares: Mapping[str, float], url: str) -> None:
    total = sum(shares.values())
    if abs(total - 100) > _LEEWAY_PERCENT:
        raise WebError(f"{url} gave shares that add up to {total:.3f}%, and not 100%")
