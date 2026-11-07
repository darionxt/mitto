"""What Chromium Dash and Google's version history answered on 2026-11-07, for tests that must not use the network.

The answers in data/ keep only the fields that the scripts read.
"""

from pathlib import Path

from tools.release.dash import DASH_URL
from tools.release.version_history import RELEASES_URL

DATA = Path(__file__).parent / "data"

_SCHEDULE_URL = f"{DASH_URL}/fetch_milestone_schedule?mstone="
_ANSWERS = {
    f"{RELEASES_URL}?filter=version%3E%3D156%2Cversion%3C159%2Cpinnable%3Dtrue": "version_history_156_to_158.json",
    f"{_SCHEDULE_URL}158": "fetch_milestone_schedule_158.json",
}


def saved(url: str) -> bytes:
    """Return the answer to a URL on 2026-11-07."""
    if url not in _ANSWERS:
        raise AssertionError(f"no saved answer for {url}")
    return (DATA / _ANSWERS[url]).read_bytes()
