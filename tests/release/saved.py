"""What web services answered, for tests that must not use the network.

Chromium Dash and Google's version history answered on 2026-11-07, StatCounter and the Firefox Public Data Report on
2026-11-08, and Chromium's Gitiles servers, the Chrome Releases blog and the version history of Chrome 157 on 2026-11-10.
The answers in data/ keep only the fields that the scripts read: the Gitiles logs keep each commit's first line and its
Bug: lines, .gitmodules the submodules that the tests name, and the blog's feed the three posts that list the fixes the
tests name, with each line of a fix up to its reporter and the sentence on an exploit in the wild. StatCounter's and Firefox's are under
CC BY-SA 3.0: StatCounter Global Stats, https://gs.statcounter.com/, and the Firefox Public Data Report by Mozilla,
https://data.firefox.com/dashboard/hardware.
"""

from pathlib import Path

from tools.release.chrome_releases import FEED_URL
from tools.release.dash import DASH_URL
from tools.release.gitiles import CHROMIUM_URL
from tools.release.machine_sources import FIREFOX_DATA_URL, STATCOUNTER_CHART_URL
from tools.release.version_history import RELEASES_URL

DATA = Path(__file__).parent / "data"

_SCHEDULE_URL = f"{DASH_URL}/fetch_milestone_schedule?mstone="

ANGLE_URL = "https://chromium.googlesource.com/angle/angle"
"""Where ANGLE's repository is, which 157.0.8103.38 rolls to a new commit."""


def gitiles_log_url(repository: str, old: str, new: str) -> str:
    """Return the address that asks a Gitiles server for the commits between two commits or tags, with their changes."""
    return f"{repository}/+log/{old}..{new}?format=JSON&name-status=1&n=1000"


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
    f"{RELEASES_URL}?filter=version%3E%3D157%2Cversion%3C158": "version_history_157.json",
    **{
        gitiles_log_url(CHROMIUM_URL, old, new): f"gitiles_src_{old}..{new}.json"
        for build in ("157.0.8103.38", "157.0.8103.39")
        for old, new in (("157.0.8103.27", build), (build, "157.0.8103.27"))
    },
    gitiles_log_url(
        ANGLE_URL, "7bf673433dd1a7602a9323608d7e3fb1583dfd1e", "751417514a521b2e92d6928260c62122138824d3"
    ): "gitiles_angle_roll_157.0.8103.38.json",
    gitiles_log_url(
        ANGLE_URL, "751417514a521b2e92d6928260c62122138824d3", "7bf673433dd1a7602a9323608d7e3fb1583dfd1e"
    ): "gitiles_angle_roll_157.0.8103.38_back.json",
    **{
        f"{CHROMIUM_URL}/+/{build}/.gitmodules?format=TEXT": f"gitmodules_{build}.txt"
        for build in ("157.0.8103.38", "157.0.8103.39")
    },
    f"{FEED_URL}?max-results=60": "chrome_releases_stable_updates.xml",
}


def saved(url: str) -> bytes:
    """Return the answer to a URL on the day it was saved."""
    if url not in _ANSWERS:
        raise AssertionError(f"no saved answer for {url}")
    return (DATA / _ANSWERS[url]).read_bytes()
