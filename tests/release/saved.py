"""What web services answered, for tests that must not use the network.

Chromium Dash and Google's version history answered on 2026-11-07, and StatCounter and the Firefox Public Data Report on
2026-11-08. The answers in data/ keep only the fields that the scripts read. StatCounter's and Firefox's are under
CC BY-SA 3.0: StatCounter Global Stats, https://gs.statcounter.com/, and the Firefox Public Data Report by Mozilla,
https://data.firefox.com/dashboard/hardware.
"""

from pathlib import Path

from tools.release.dash import DASH_URL
from tools.release.machine_sources import FIREFOX_DATA_URL, STATCOUNTER_CHART_URL
from tools.release.version_history import RELEASES_URL

DATA = Path(__file__).parent / "data"

_SCHEDULE_URL = f"{DASH_URL}/fetch_milestone_schedule?mstone="


def statcounter_url(key: str, name: str, month: str) -> str:
    """Return the link that StatCounter's page of a statistic downloads its CSV of a month from, such as "2026-10"."""
    return (
        f"{STATCOUNTER_CHART_URL}?device=Desktop&device_hidden=desktop&statType_hidden={key}&region_hidden=ww"
        f"&granularity=monthly&statType={name}&region=Worldwide&fromInt={month.replace('-', '')}"
        f"&toInt={month.replace('-', '')}&fromMonthYear={month}&toMonthYear={month}&csv=1"
    )


_ANSWERS = {
    f"{RELEASES_URL}?filter=version%3E%3D156%2Cversion%3C159%2Cpinnable%3Dtrue": "version_history_156_to_158.json",
    f"{_SCHEDULE_URL}158": "fetch_milestone_schedule_158.json",
    **{
        statcounter_url(key, name, month): f"statcounter_{key}_{month}.csv"
        for key, name in (("windows_version", "Windows%20Version"), ("resolution", "Screen%20Resolution"))
        for month in ("2026-08", "2026-09", "2026-10")
    },
    f"{FIREFOX_DATA_URL}/resolution/index.json": "firefox_resolution.json",
    f"{FIREFOX_DATA_URL}/ram/index.json": "firefox_ram.json",
}


def saved(url: str) -> bytes:
    """Return the answer to a URL on the day it was saved."""
    if url not in _ANSWERS:
        raise AssertionError(f"no saved answer for {url}")
    return (DATA / _ANSWERS[url]).read_bytes()
